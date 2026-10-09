// The HLE BIOS's C library: A(0Ah)-A(3Fh) - strings, memory, numbers, the heap and printf.
//
// psx-spx documents these down to their bugs - memcmp answering with the byte after the
// mismatch, memmove's backwards copy moving one byte too many, strstr not stepping back after a
// partial match, strpbrk's return when nothing matches, strtok's skipping - and they are kept,
// because software written against the console's kernel has only ever seen them that way.

#include "psx/psx.h"
#include "psx/hle_bios.h"
#include "psx/mips_assembler.h"

namespace emulation {
namespace psx {

namespace {

bool IsBlank(uint8_t c) { return (c >= 0x09 && c <= 0x0D) || c == 0x20; }

// A digit's value in bases up to 36, or 99 for anything that is not one.
uint32_t DigitValue(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
  return 99;
}

}  // namespace

// ---- The heap ----------------------------------------------------------------------------------
//
// Blocks are a header word - the size of what follows, bit 0 set while it is free - and the
// memory itself. free() is the kernel's "[buf-4] |= 1", so a game that frees by hand, or frees
// twice, gets what it would on a console. Neighbouring free blocks are merged when an allocation
// walks past them.

void HleBios::HeapInit(uint32_t start, uint32_t size) {
  start = (start + 3) & ~3u;
  size &= ~3u;
  if (size < 8)
    return;
  Write32(start, (size - 4) | 1);
}

uint32_t HleBios::HeapAlloc(uint32_t start, uint32_t end, uint32_t size) {
  size = (size + 3) & ~3u;
  if (size == 0)
    size = 4;
  start = (start + 3) & ~3u;
  uint32_t at = start;
  for (int guard = 0; at + 4 <= end && guard < 0x100000; ++guard) {
    uint32_t header = Read32(at);
    uint32_t bytes = header & ~3u;
    if (header & 1) {
      // Merge whatever free blocks follow.
      for (;;) {
        const uint32_t next = at + 4 + bytes;
        if (next + 4 > end)
          break;
        const uint32_t next_header = Read32(next);
        if ((next_header & 1) == 0)
          break;
        bytes += 4 + (next_header & ~3u);
      }
      if (at + 4 + bytes > end)
        bytes = end - at - 4;
      if (bytes >= size) {
        if (bytes - size >= 8) {
          Write32(at, size);
          Write32(at + 4 + size, (bytes - size - 4) | 1);
        } else {
          Write32(at, bytes);
        }
        return at + 4;
      }
      Write32(at, bytes | 1);
    }
    if (bytes == 0 && (header & 1) == 0)
      break;   // a corrupt header: nothing past it can be trusted
    at += 4 + bytes;
  }
  return 0;
}

// ---- Numbers -----------------------------------------------------------------------------------

// strtol, strtoul, atoi and atol, which differ only in what they make of the start of the
// number (psx-spx "BIOS Number/String/Character Conversion"). Prefixes override the base -
// "0b" binary, "0x" hex, and "o" octal, or for atoi a leading "0" - which is also why bases above
// 11 cannot work as asked.
int32_t HleBios::StrToL(uint32_t source, uint32_t end_out, int base, bool is_signed, bool atoi) {
  if (source == 0)
    return 0;
  uint32_t p = source;
  while (IsBlank(Read8(p)))
    ++p;
  bool negative = false;
  if (is_signed && Read8(p) == '-') {
    negative = true;
    ++p;
  }
  if (base < 2 || base > 36)
    base = 10;
  const uint8_t c0 = Read8(p);
  const uint8_t c1 = static_cast<uint8_t>(tolower(Read8(p + 1)));
  if (c0 == '0' && c1 == 'x') {
    base = 16;
    p += 2;
  } else if (c0 == '0' && c1 == 'b') {
    base = 2;
    p += 2;
  } else if (!atoi && (c0 == 'o' || c0 == 'O')) {
    base = 8;
    p += 1;
  } else if (atoi && c0 == '0') {
    base = 8;
    p += 1;
  }
  uint32_t result = 0;
  for (;;) {
    const uint32_t digit = DigitValue(Read8(p));
    if (digit >= static_cast<uint32_t>(base))
      break;
    result = result * base + digit;
    ++p;
  }
  if (end_out != 0)
    Write32(end_out, p);
  return negative ? -static_cast<int32_t>(result) : static_cast<int32_t>(result);
}

// ---- printf ------------------------------------------------------------------------------------

// The kernel's printf, onto the BIOS console: the usual conversions, flags, width, precision and
// the l/h modifiers, with arguments from a1 onwards and then the caller's stack.
void HleBios::Printf(uint32_t format, int first_argument) {
  std::string out;
  int argument = first_argument;
  uint32_t p = format;
  for (int guard = 0; guard < 4096; ++guard) {
    const char c = static_cast<char>(Read8(p++));
    if (c == '\0')
      break;
    if (c != '%') {
      out.push_back(c);
      continue;
    }
    std::string spec = "%";
    char conversion = '\0';
    for (int i = 0; i < 16; ++i) {
      const char s = static_cast<char>(Read8(p++));
      if (s == '\0') {
        --p;
        break;
      }
      if (strchr("-+ #0123456789.", s) != nullptr) {
        spec.push_back(s);
        continue;
      }
      if (s == '*') {
        spec += std::to_string(static_cast<int32_t>(Arg(argument++)));
        continue;
      }
      if (s == 'l' || s == 'h')
        continue;   // everything is 32 bits here
      conversion = s;
      break;
    }
    char buffer[512];
    buffer[0] = '\0';
    switch (conversion) {
      case 'd': case 'i':
        snprintf(buffer, sizeof(buffer), (spec + "d").c_str(),
                 static_cast<int32_t>(Arg(argument++)));
        break;
      case 'u': case 'x': case 'X': case 'o':
        snprintf(buffer, sizeof(buffer), (spec + conversion).c_str(), Arg(argument++));
        break;
      case 'p':
        snprintf(buffer, sizeof(buffer), "%08x", Arg(argument++));
        break;
      case 'c':
        snprintf(buffer, sizeof(buffer), (spec + "c").c_str(), static_cast<int>(Arg(argument++) & 0xFF));
        break;
      case 's': {
        const uint32_t s = Arg(argument++);
        const std::string text = s != 0 ? ReadString(s, 400) : std::string("(null)");
        snprintf(buffer, sizeof(buffer), (spec + "s").c_str(), text.c_str());
        break;
      }
      case '%':
        buffer[0] = '%';
        buffer[1] = '\0';
        break;
      case '\0':
        break;
      default:
        // %f, %e, %g: the kernel's would need the FPU this machine has not got.
        ++argument;
        break;
    }
    out += buffer;
  }
  Console(out);
}

// ---- qsort, lsearch, bsearch ---------------------------------------------------------------------
//
// Each compare is a call into the game, so each is a loop that stops at every compare and resumes
// with its result (HleBios::CallGuest). qsort is an insertion sort - the kernel's own is a quick
// sort that parks an element on the heap; the order of equal elements is the one thing a caller
// could tell apart, and qsort promises nothing about it.
void HleBios::SearchStep(Frame frame, bool first, int32_t result) {
  CpuContext& c = cpu();
  auto compare = [&](uint32_t a, uint32_t b, uint32_t callback) {
    c.gp.a0 = a;
    c.gp.a1 = b;
    CallGuest(callback, frame);
  };
  switch (frame.kind) {
    case kFrameQsort: {
      // w: base, nel, width, callback, i, j, return address
      const uint32_t base = frame.w[0], count = frame.w[1], width = frame.w[2];
      uint32_t& i = frame.w[4];
      uint32_t& j = frame.w[5];
      if (first) {
        i = 1;
        j = 1;
      } else {
        bool advance = true;
        if (result > 0) {
          // Out of order: swap j-1 and j, and keep sinking.
          std::vector<uint8_t> x(width), y(width);
          ReadBlock(base + (j - 1) * width, &x[0], width);
          ReadBlock(base + j * width, &y[0], width);
          WriteBlock(base + (j - 1) * width, &y[0], width);
          WriteBlock(base + j * width, &x[0], width);
          if (--j > 0)
            advance = false;
        }
        if (advance) {
          ++i;
          j = i;
        }
      }
      if (width == 0 || i >= count) {
        c.pc = frame.w[6];
        return;
      }
      compare(base + (j - 1) * width, base + j * width, frame.w[3]);
      return;
    }
    case kFrameLsearch: {
      // w: key, base, nel, width, callback, i, return address
      uint32_t& i = frame.w[5];
      if (first) {
        i = 0;
      } else if (result == 0) {
        c.gp.v0 = frame.w[1] + i * frame.w[3];
        c.pc = frame.w[6];
        return;
      } else {
        ++i;
      }
      if (i >= frame.w[2]) {
        c.gp.v0 = 0;
        c.pc = frame.w[6];
        return;
      }
      compare(frame.w[0], frame.w[1] + i * frame.w[3], frame.w[4]);
      return;
    }
    case kFrameBsearch: {
      // w: key, base, width, callback, low, high (one past), return address
      uint32_t& low = frame.w[4];
      uint32_t& high = frame.w[5];
      if (!first) {
        const uint32_t middle = low + (high - low) / 2;
        if (result == 0) {
          c.gp.v0 = frame.w[1] + middle * frame.w[2];
          c.pc = frame.w[6];
          return;
        }
        if (result < 0)
          high = middle;
        else
          low = middle + 1;
      }
      if (low >= high) {
        c.gp.v0 = 0;
        c.pc = frame.w[6];
        return;
      }
      const uint32_t middle = low + (high - low) / 2;
      compare(frame.w[0], frame.w[1] + middle * frame.w[2], frame.w[3]);
      return;
    }
    default:
      SystemError("a search with no kind");
      return;
  }
}

// ---- The functions -------------------------------------------------------------------------------

bool HleBios::LibcFunction(uint32_t n) {
  CpuContext& c = cpu();
  KernelVars& v = vars();
  switch (n) {
    case 0x0A: {   // todigit(char)
      const uint8_t ch = static_cast<uint8_t>(Arg(0));
      const uint32_t digit = DigitValue(ch);
      Return(digit == 99 ? 0x0098967F : digit);
      return true;
    }
    case 0x0B:     // atof: compiled for a coprocessor 1 the machine has not got
    case 0x32:     // strtod: likewise
      SystemError("atof/strtod: coprocessor 1 unusable");
      return true;
    case 0x0C:     // strtoul(src, src_end, base)
      Return(static_cast<uint32_t>(StrToL(Arg(0), Arg(1), static_cast<int>(Arg(2)), false, false)));
      return true;
    case 0x0D:     // strtol(src, src_end, base)
      Return(static_cast<uint32_t>(StrToL(Arg(0), Arg(1), static_cast<int>(Arg(2)), true, false)));
      return true;
    case 0x0E:     // abs
    case 0x0F: {   // labs
      const int32_t value = static_cast<int32_t>(Arg(0));
      Return(static_cast<uint32_t>(value < 0 ? -value : value));
      return true;
    }
    case 0x10:     // atoi
    case 0x11:     // atol
      Return(static_cast<uint32_t>(StrToL(Arg(0), 0, 10, true, true)));
      return true;
    case 0x12: {   // atob(src, num_dst): strtol's two answers, swapped
      const uint32_t end = c.gp.sp - 0x10;
      Write32(end, Arg(0));
      const int32_t value = StrToL(Arg(0), end, 10, true, false);
      Write32(Arg(1), static_cast<uint32_t>(value));
      Return(Read32(end));
      return true;
    }
    case 0x13: {   // SaveState(buf): setjmp
      const uint32_t buf = Arg(0);
      Write32(buf + 0x00, c.gp.ra);
      Write32(buf + 0x04, c.gp.sp);
      Write32(buf + 0x08, c.gp.fp);
      for (uint32_t r = 0; r < 8; ++r)
        Write32(buf + 0x0C + r * 4, c.gp.reg[kS0 + r]);
      Write32(buf + 0x2C, c.gp.gp);
      Return(0);
      return true;
    }
    case 0x14: {   // RestoreState(buf, param): longjmp
      const uint32_t buf = Arg(0);
      const uint32_t param = Arg(1);
      c.gp.ra = Read32(buf + 0x00);
      c.gp.sp = Read32(buf + 0x04);
      c.gp.fp = Read32(buf + 0x08);
      for (uint32_t r = 0; r < 8; ++r)
        c.gp.reg[kS0 + r] = Read32(buf + 0x0C + r * 4);
      c.gp.gp = Read32(buf + 0x2C);
      Return(param);
      return true;
    }
    case 0x15: {   // strcat(dst, src)
      const uint32_t dst = Arg(0), src = Arg(1);
      if (dst == 0 || src == 0) {
        Return(0);
        return true;
      }
      const std::string tail = ReadString(src, 0x100000);
      uint32_t end = dst;
      while (Read8(end) != 0)
        ++end;
      WriteString(end, tail);
      Return(dst);
      return true;
    }
    case 0x16: {   // strncat(dst, src, maxlen)
      const uint32_t dst = Arg(0), src = Arg(1);
      const int32_t maxlen = static_cast<int32_t>(Arg(2));
      if (dst == 0 || src == 0) {
        Return(0);
        return true;
      }
      uint32_t end = dst;
      while (Read8(end) != 0)
        ++end;
      const uint32_t limit = maxlen > 0 ? static_cast<uint32_t>(maxlen) : 0;
      std::string tail = ReadString(src, limit);
      WriteString(end, tail);
      Return(dst);
      return true;
    }
    case 0x17:     // strcmp(str1, str2)
    case 0x18: {   // strncmp(str1, str2, maxlen)
      const uint32_t a = Arg(0), b = Arg(1);
      const uint32_t limit = n == 0x18 ? Arg(2) : 0xFFFFFFFF;
      if (a == 0 || b == 0) {
        Return(a == 0 && b == 0 ? 0 : (a == 0 ? 0xFFFFFFFF : 1));
        return true;
      }
      for (uint32_t i = 0; i < limit; ++i) {
        const int8_t x = static_cast<int8_t>(Read8(a + i));
        const int8_t y = static_cast<int8_t>(Read8(b + i));
        if (x != y) {
          Return(static_cast<uint32_t>(static_cast<int32_t>(x) - static_cast<int32_t>(y)));
          return true;
        }
        if (x == 0)
          break;
      }
      Return(0);
      return true;
    }
    case 0x19: {   // strcpy(dst, src)
      const uint32_t dst = Arg(0), src = Arg(1);
      if (dst == 0 || src == 0) {
        Return(0);
        return true;
      }
      WriteString(dst, ReadString(src, 0x100000));
      Return(dst);
      return true;
    }
    case 0x1A: {   // strncpy(dst, src, maxlen)
      const uint32_t dst = Arg(0), src = Arg(1), maxlen = Arg(2);
      if (dst == 0 || src == 0) {
        Return(0);
        return true;
      }
      bool ended = false;
      for (uint32_t i = 0; i < maxlen; ++i) {
        const uint8_t ch = ended ? 0 : Read8(src + i);
        if (ch == 0)
          ended = true;
        Write8(dst + i, ch);
      }
      Return(dst);
      return true;
    }
    case 0x1B: {   // strlen(src)
      const uint32_t src = Arg(0);
      uint32_t length = 0;
      if (src != 0)
        while (Read8(src + length) != 0)
          ++length;
      Return(length);
      return true;
    }
    case 0x1C: case 0x1E:     // index / strchr(src, char)
    case 0x1D: case 0x1F: {   // rindex / strrchr(src, char)
      const uint32_t src = Arg(0);
      const uint8_t wanted = static_cast<uint8_t>(Arg(1));
      if (src == 0) {
        Return(0);
        return true;
      }
      uint32_t found = 0;
      for (uint32_t p = src;; ++p) {
        const uint8_t ch = Read8(p);
        if (ch == wanted) {
          found = p;
          if (n == 0x1C || n == 0x1E)
            break;
        }
        if (ch == 0)
          break;
      }
      Return(found);
      return true;
    }
    case 0x20: {   // strpbrk(src, list)
      const uint32_t src = Arg(0);
      const std::string list = ReadString(Arg(1));
      for (uint32_t p = src;; ++p) {
        const char ch = static_cast<char>(Read8(p));
        if (ch == 0)
          break;
        if (list.find(ch) != std::string::npos) {
          Return(p);
          return true;
        }
      }
      // Nothing found: 0 only for an empty string, the string itself otherwise.
      Return(Read8(src) == 0 ? 0 : src);
      return true;
    }
    case 0x21:     // strspn(src, list)
    case 0x22: {   // strcspn(src, list)
      const uint32_t src = Arg(0);
      const std::string list = ReadString(Arg(1));
      uint32_t i = 0;
      for (;; ++i) {
        const char ch = static_cast<char>(Read8(src + i));
        if (ch == 0)
          break;
        const bool in = list.find(ch) != std::string::npos;
        if (n == 0x21 ? !in : in)
          break;
      }
      Return(i);
      return true;
    }
    case 0x23: {   // strtok(src, list)
      const uint32_t src = Arg(0);
      const std::string list = ReadString(Arg(1));
      if (src != 0) {
        const std::string text = ReadString(src, 0xFF);
        WriteString(kStrtokBuffer, text);
        v.strtok_next = kStrtokBuffer;
      }
      uint32_t p = v.strtok_next;
      if (p == 0 || Read8(p) == 0) {
        v.strtok_next = 0;
        Return(0);
        return true;
      }
      const uint32_t token = p;
      while (Read8(p) != 0 && list.find(static_cast<char>(Read8(p))) == std::string::npos)
        ++p;
      if (Read8(p) == 0) {
        v.strtok_next = 0;
      } else {
        Write8(p, 0);
        ++p;
        // The skip of repeated separators only works with a single one in the list.
        if (list.size() == 1)
          while (Read8(p) == static_cast<uint8_t>(list[0]))
            ++p;
        v.strtok_next = p;
      }
      Return(token);
      return true;
    }
    case 0x24: {   // strstr(str, substr): carries on from the mismatch, never steps back
      const uint32_t str = Arg(0), sub = Arg(1);
      if (str == 0 || sub == 0) {
        Return(0);
        return true;
      }
      uint32_t s = str;
      while (Read8(s) != 0) {
        uint32_t a = s, b = sub;
        while (Read8(b) != 0 && Read8(a) == Read8(b)) {
          ++a;
          ++b;
        }
        if (Read8(b) == 0) {
          Return(s);
          return true;
        }
        s = a + 1;
      }
      Return(0);
      return true;
    }
    case 0x25: {   // toupper
      const uint32_t ch = Arg(0) & 0xFF;
      Return(ch >= 'a' && ch <= 'z' ? ch - 0x20 : ch);
      return true;
    }
    case 0x26: {   // tolower
      const uint32_t ch = Arg(0) & 0xFF;
      Return(ch >= 'A' && ch <= 'Z' ? ch + 0x20 : ch);
      return true;
    }
    case 0x27:     // bcopy(src, dst, len)
    case 0x2A: {   // memcpy(dst, src, len)
      const uint32_t dst = n == 0x27 ? Arg(1) : Arg(0);
      const uint32_t src = n == 0x27 ? Arg(0) : Arg(1);
      const uint32_t len = Arg(2);
      const uint32_t refuse = n == 0x27 ? src : dst;
      if (refuse != 0 && len <= 0x7FFFFFFF && len != 0) {
        std::vector<uint8_t> data(len);
        ReadBlock(src, &data[0], len);
        WriteBlock(dst, &data[0], len);
      }
      Return(n == 0x27 ? src : dst);
      return true;
    }
    case 0x28:     // bzero(dst, len)
    case 0x2B: {   // memset(dst, fillbyte, len)
      const uint32_t dst = Arg(0);
      const uint8_t fill = n == 0x2B ? static_cast<uint8_t>(Arg(1)) : 0;
      const uint32_t len = n == 0x2B ? Arg(2) : Arg(1);
      if (dst == 0 || len == 0 || len > 0x7FFFFFFF) {
        Return(0);
        return true;
      }
      FillBlock(dst, fill, len);
      Return(dst);
      return true;
    }
    case 0x29:     // bcmp(ptr1, ptr2, len)
    case 0x2D: {   // memcmp(src1, src2, len): the bytes after the mismatch, not at it
      const uint32_t a = Arg(0), b = Arg(1), len = Arg(2);
      if (a == 0 || b == 0) {
        Return(0);
        return true;
      }
      for (uint32_t i = 0; i < len; ++i) {
        if (Read8(a + i) != Read8(b + i)) {
          Return(static_cast<uint32_t>(static_cast<int32_t>(Read8(a + i + 1)) -
                                       static_cast<int32_t>(Read8(b + i + 1))));
          return true;
        }
      }
      Return(0);
      return true;
    }
    case 0x2C: {   // memmove(dst, src, len)
      const uint32_t dst = Arg(0), src = Arg(1), len = Arg(2);
      if (dst == 0 || len > 0x7FFFFFFF) {
        Return(dst);
        return true;
      }
      if (src < dst && dst >= src + len) {
        // Backwards, and one byte too many: [src+len..src] to [dst+len..dst].
        for (uint32_t i = len + 1; i-- > 0;)
          Write8(dst + i, Read8(src + i));
      } else if (len != 0) {
        std::vector<uint8_t> data(len);
        ReadBlock(src, &data[0], len);
        WriteBlock(dst, &data[0], len);
      }
      Return(dst);
      return true;
    }
    case 0x2E: {   // memchr(src, scanbyte, len)
      const uint32_t src = Arg(0), len = Arg(2);
      const uint8_t wanted = static_cast<uint8_t>(Arg(1));
      if (src == 0 || len > 0x7FFFFFFF) {
        Return(0);
        return true;
      }
      for (uint32_t i = 0; i < len; ++i)
        if (Read8(src + i) == wanted) {
          Return(src + i);
          return true;
        }
      Return(0);
      return true;
    }
    case 0x2F:     // rand
      v.rand_seed = v.rand_seed * 0x41C64E6D + 0x3039;
      Return((v.rand_seed >> 16) & 0x7FFF);
      return true;
    case 0x30:     // srand(seed)
      v.rand_seed = Arg(0);
      ReturnVoid();
      return true;
    case 0x31: {   // qsort(base, nel, width, callback)
      Frame frame = {};
      frame.kind = kFrameQsort;
      frame.w[0] = Arg(0);
      frame.w[1] = Arg(1);
      frame.w[2] = Arg(2);
      frame.w[3] = Arg(3);
      frame.w[6] = c.gp.ra;
      SearchStep(frame, true, 0);
      return true;
    }
    case 0x35: {   // lsearch(key, base, nel, width, callback)
      Frame frame = {};
      frame.kind = kFrameLsearch;
      for (int i = 0; i < 5; ++i)
        frame.w[i] = Arg(i);
      frame.w[6] = c.gp.ra;
      SearchStep(frame, true, 0);
      return true;
    }
    case 0x36: {   // bsearch(key, base, nel, width, callback)
      Frame frame = {};
      frame.kind = kFrameBsearch;
      frame.w[0] = Arg(0);
      frame.w[1] = Arg(1);
      frame.w[2] = Arg(3);
      frame.w[3] = Arg(4);
      frame.w[4] = 0;
      frame.w[5] = Arg(2);
      frame.w[6] = c.gp.ra;
      SearchStep(frame, true, 0);
      return true;
    }
    case 0x33:     // malloc(size)
      Return(HeapAlloc(v.heap_start, v.heap_end, Arg(0)));
      return true;
    case 0x34:     // free(buf)
      if (Arg(0) != 0)
        Write32(Arg(0) - 4, Read32(Arg(0) - 4) | 1);
      ReturnVoid();
      return true;
    case 0x37: {   // calloc(sizx, sizy)
      const uint32_t bytes = Arg(0) * Arg(1);
      const uint32_t block = HeapAlloc(v.heap_start, v.heap_end, bytes);
      if (block != 0)
        FillBlock(block, 0, bytes);
      Return(block);
      return true;
    }
    case 0x38: {   // realloc(old_buf, new_size): copies new_size bytes, whatever the old one held
      const uint32_t old = Arg(0), size = Arg(1);
      if (old == 0) {
        Return(HeapAlloc(v.heap_start, v.heap_end, size));
        return true;
      }
      if (size == 0) {
        Write32(old - 4, Read32(old - 4) | 1);
        Return(0);
        return true;
      }
      const uint32_t block = HeapAlloc(v.heap_start, v.heap_end, size);
      if (block != 0) {
        std::vector<uint8_t> data(size);
        ReadBlock(old, &data[0], size);
        WriteBlock(block, &data[0], size);
        Write32(old - 4, Read32(old - 4) | 1);
      }
      Return(block);
      return true;
    }
    case 0x39:     // InitHeap(addr, size)
      v.heap_start = Arg(0);
      v.heap_end = Arg(0) + Arg(1);
      HeapInit(Arg(0), Arg(1));
      ReturnVoid();
      return true;
    case 0x3F:     // printf(txt, ...)
      Printf(Arg(0), 1);
      Return(0);
      return true;
    default:
      return false;
  }
}

}  // namespace psx
}  // namespace emulation
