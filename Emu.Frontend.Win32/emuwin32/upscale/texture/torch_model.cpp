/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
#include "emuwin32/upscale/texture/torch_model.h"

#include <DirectXPackedVector.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <set>

namespace emulation::win32 {

    namespace {

        // ---- A pickle, read as data ------------------------------------------------------------

        struct Value;
        typedef std::shared_ptr<Value> Ref;

        struct Value {
            enum Kind { kMark, kNone, kBool, kInt, kFloat, kString, kTuple, kList, kDict, kGlobal,
                        kStorage, kTensor, kObject };
            Kind kind = kNone;
            int64_t number = 0;
            double real = 0.0;
            std::string text;          // a string; a global's "module.name"; a storage's key
            std::vector<Ref> items;    // a tuple's or list's; a dict's keys and values in turn
            // A storage: its element type ("float", "half", "bfloat16", "double").
            std::string dtype;
            // A tensor: its storage, where in it it starts (in elements), its shape and strides.
            Ref storage;
            int64_t offset = 0;
            std::vector<int64_t> shape, stride;
        };

        Ref Make(Value::Kind kind) {
            Ref value = std::make_shared<Value>();
            value->kind = kind;
            return value;
        }

        class Unpickler {
         public:
            // `persistent` turns a persistent id - PyTorch's way of naming a storage - into a value.
            Unpickler(const uint8_t* data, size_t size, std::function<Ref(const Ref&)> persistent)
                : data_(data), size_(size), persistent_(persistent) {}

            size_t position() const { return at_; }
            void set_position(size_t at) { at_ = at; }

            // One pickle, to its STOP. Null, with `error`, on anything it does not know.
            Ref Load(std::string* error) {
                stack_.clear();
                memo_.clear();
                while (at_ < size_) {
                    const uint8_t op = data_[at_++];
                    if (!Step(op, error))
                        return nullptr;
                    if (op == '.') {
                        if (stack_.empty()) {
                            *error = "a pickle ended with nothing on its stack";
                            return nullptr;
                        }
                        return stack_.back();
                    }
                }
                *error = "a pickle runs past the end of the file";
                return nullptr;
            }

         private:
            bool Need(size_t bytes, std::string* error) {
                if (size_ - at_ < bytes) {
                    *error = "a pickle runs past the end of the file";
                    return false;
                }
                return true;
            }
            uint64_t Unsigned(int bytes) {
                uint64_t value = 0;
                for (int i = 0; i < bytes; ++i)
                    value |= static_cast<uint64_t>(data_[at_ + i]) << (8 * i);
                at_ += bytes;
                return value;
            }
            std::string Bytes(size_t length) {
                std::string text(reinterpret_cast<const char*>(data_ + at_), length);
                at_ += length;
                return text;
            }
            bool Line(std::string* text, std::string* error) {
                const void* end = memchr(data_ + at_, '\n', size_ - at_);
                if (end == nullptr) {
                    *error = "a pickle's global has no end";
                    return false;
                }
                const size_t length = static_cast<const uint8_t*>(end) - (data_ + at_);
                *text = Bytes(length);
                ++at_;
                return true;
            }
            Ref Pop() {
                if (stack_.empty())
                    return nullptr;
                Ref top = stack_.back();
                stack_.pop_back();
                return top;
            }
            // Everything above the last mark, and the mark gone.
            bool PopToMark(std::vector<Ref>* items) {
                for (size_t i = stack_.size(); i-- > 0;) {
                    if (stack_[i]->kind == Value::kMark) {
                        items->assign(stack_.begin() + i + 1, stack_.end());
                        stack_.resize(i);
                        return true;
                    }
                }
                return false;
            }
            void Push(const Ref& value) { stack_.push_back(value); }
            void PushInt(int64_t number) {
                Ref value = Make(Value::kInt);
                value->number = number;
                Push(value);
            }
            void PushString(const std::string& text) {
                Ref value = Make(Value::kString);
                value->text = text;
                Push(value);
            }
            void PushTuple(std::vector<Ref> items) {
                Ref value = Make(Value::kTuple);
                value->items = std::move(items);
                Push(value);
            }

            // What calling `callable` with `args` gives - for the few callables a state dict
            // needs, worked out here; anything else an inert object named after it.
            Ref Apply(const Ref& callable, const Ref& args) {
                const std::string name = callable->kind == Value::kGlobal ? callable->text : "";
                const std::vector<Ref> none;
                const std::vector<Ref>& a = args != nullptr ? args->items : none;
                if (name == "collections.OrderedDict" || name == "builtins.dict")
                    return Make(Value::kDict);
                if ((name == "torch._utils._rebuild_tensor_v2" ||
                     name == "torch._utils._rebuild_tensor") &&
                    a.size() >= 4 && a[0] != nullptr && a[0]->kind == Value::kStorage) {
                    Ref tensor = Make(Value::kTensor);
                    tensor->storage = a[0];
                    tensor->offset = a[1]->number + a[0]->offset;
                    for (const Ref& d : a[2]->items)
                        tensor->shape.push_back(d->number);
                    for (const Ref& s : a[3]->items)
                        tensor->stride.push_back(s->number);
                    return tensor;
                }
                if (name == "torch._utils._rebuild_parameter" && !a.empty())
                    return a[0];
                Ref object = Make(Value::kObject);
                object->text = name;
                return object;
            }

            bool Step(uint8_t op, std::string* error) {
                switch (op) {
                case 0x80:   // PROTO
                    if (!Need(1, error)) return false;
                    ++at_;
                    return true;
                case 0x95:   // FRAME
                    if (!Need(8, error)) return false;
                    at_ += 8;
                    return true;
                case 'c': {  // GLOBAL
                    std::string module, name;
                    if (!Line(&module, error) || !Line(&name, error))
                        return false;
                    Ref global = Make(Value::kGlobal);
                    global->text = module + "." + name;
                    Push(global);
                    return true;
                }
                case 0x93: {   // STACK_GLOBAL
                    Ref name = Pop(), module = Pop();
                    if (name == nullptr || module == nullptr) break;
                    Ref global = Make(Value::kGlobal);
                    global->text = module->text + "." + name->text;
                    Push(global);
                    return true;
                }
                case '}': Push(Make(Value::kDict)); return true;
                case ']': Push(Make(Value::kList)); return true;
                case ')': PushTuple({}); return true;
                case '(': Push(Make(Value::kMark)); return true;
                case 't': {
                    std::vector<Ref> items;
                    if (!PopToMark(&items)) break;
                    PushTuple(std::move(items));
                    return true;
                }
                case 0x85: case 0x86: case 0x87: {   // TUPLE1-3
                    const size_t n = op - 0x84;
                    if (stack_.size() < n) break;
                    std::vector<Ref> items(stack_.end() - n, stack_.end());
                    stack_.resize(stack_.size() - n);
                    PushTuple(std::move(items));
                    return true;
                }
                case 'l': case 'd': {   // LIST, DICT
                    std::vector<Ref> items;
                    if (!PopToMark(&items)) break;
                    Ref value = Make(op == 'l' ? Value::kList : Value::kDict);
                    value->items = std::move(items);
                    Push(value);
                    return true;
                }
                case 'q': case 'r': case 0x94: {   // BINPUT, LONG_BINPUT, MEMOIZE
                    uint32_t index = static_cast<uint32_t>(memo_.size());
                    if (op != 0x94) {
                        if (!Need(op == 'q' ? 1 : 4, error)) return false;
                        index = static_cast<uint32_t>(Unsigned(op == 'q' ? 1 : 4));
                    }
                    if (stack_.empty()) break;
                    memo_[index] = stack_.back();
                    return true;
                }
                case 'h': case 'j': {   // BINGET, LONG_BINGET
                    if (!Need(op == 'h' ? 1 : 4, error)) return false;
                    const uint32_t index = static_cast<uint32_t>(Unsigned(op == 'h' ? 1 : 4));
                    auto found = memo_.find(index);
                    if (found == memo_.end()) break;
                    Push(found->second);
                    return true;
                }
                case 'X': case 0x8c: case 0x8d: case 'T': case 'U': case 'B': case 'C': {
                    const int width = (op == 0x8c || op == 'U' || op == 'C') ? 1 : op == 0x8d ? 8 : 4;
                    if (!Need(width, error)) return false;
                    const uint64_t length = Unsigned(width);
                    if (!Need(length, error)) return false;
                    PushString(Bytes(static_cast<size_t>(length)));
                    return true;
                }
                case 'J':
                    if (!Need(4, error)) return false;
                    PushInt(static_cast<int32_t>(Unsigned(4)));
                    return true;
                case 'K':
                    if (!Need(1, error)) return false;
                    PushInt(static_cast<int64_t>(Unsigned(1)));
                    return true;
                case 'M':
                    if (!Need(2, error)) return false;
                    PushInt(static_cast<int64_t>(Unsigned(2)));
                    return true;
                case 0x8a: case 0x8b: {   // LONG1, LONG4: little-endian two's complement
                    if (!Need(op == 0x8a ? 1 : 4, error)) return false;
                    const uint64_t length = Unsigned(op == 0x8a ? 1 : 4);
                    if (!Need(length, error)) return false;
                    int64_t value = 0;
                    for (uint64_t i = 0; i < length && i < 8; ++i)
                        value |= static_cast<int64_t>(data_[at_ + i]) << (8 * i);
                    if (length > 0 && length < 8 && (data_[at_ + length - 1] & 0x80) != 0)
                        value -= static_cast<int64_t>(1) << (8 * length);
                    at_ += static_cast<size_t>(length);
                    PushInt(value);
                    return true;
                }
                case 'N': Push(Make(Value::kNone)); return true;
                case 0x88: case 0x89: {
                    Ref value = Make(Value::kBool);
                    value->number = op == 0x88;
                    Push(value);
                    return true;
                }
                case 'G': {   // BINFLOAT, big-endian
                    if (!Need(8, error)) return false;
                    uint64_t bits = 0;
                    for (int i = 0; i < 8; ++i)
                        bits = (bits << 8) | data_[at_ + i];
                    at_ += 8;
                    Ref value = Make(Value::kFloat);
                    memcpy(&value->real, &bits, 8);
                    Push(value);
                    return true;
                }
                case 'R': case 0x81: {   // REDUCE, NEWOBJ
                    Ref args = Pop(), callable = Pop();
                    if (args == nullptr || callable == nullptr) break;
                    Push(Apply(callable, args));
                    return true;
                }
                case 'b':   // BUILD: an object's state, which nothing here needs
                    if (Pop() == nullptr || stack_.empty()) break;
                    return true;
                case 's': {
                    Ref value = Pop(), key = Pop();
                    if (value == nullptr || key == nullptr || stack_.empty()) break;
                    stack_.back()->items.push_back(key);
                    stack_.back()->items.push_back(value);
                    return true;
                }
                case 'u': case 'e': {   // SETITEMS, APPENDS
                    std::vector<Ref> items;
                    if (!PopToMark(&items) || stack_.empty()) break;
                    Ref& target = stack_.back();
                    target->items.insert(target->items.end(), items.begin(), items.end());
                    return true;
                }
                case 'a': {
                    Ref value = Pop();
                    if (value == nullptr || stack_.empty()) break;
                    stack_.back()->items.push_back(value);
                    return true;
                }
                case 'Q': {   // BINPERSID
                    Ref id = Pop();
                    if (id == nullptr) break;
                    Ref value = persistent_(id);
                    if (value == nullptr) {
                        *error = "a storage the file names could not be read";
                        return false;
                    }
                    Push(value);
                    return true;
                }
                case '.':
                    return true;
                default: {
                    char text[64];
                    snprintf(text, sizeof(text), "an unknown pickle opcode 0x%02x", op);
                    *error = text;
                    return false;
                }
                }
                *error = "a pickle's stack does not hold what it should";
                return false;
            }

            const uint8_t* data_;
            size_t size_;
            size_t at_ = 0;
            std::function<Ref(const Ref&)> persistent_;
            std::vector<Ref> stack_;
            std::map<uint32_t, Ref> memo_;
        };

        // A persistent id - ('storage', torch.FloatStorage, key, location, size[, view]) - as a
        // storage value; null for anything else.
        Ref Storage(const Ref& id) {
            if (id->kind != Value::kTuple || id->items.size() < 5 ||
                id->items[0]->text != "storage" || id->items[1]->kind != Value::kGlobal)
                return nullptr;
            const std::string& type = id->items[1]->text;
            Ref storage = Make(Value::kStorage);
            if (type == "torch.FloatStorage")
                storage->dtype = "float";
            else if (type == "torch.HalfStorage")
                storage->dtype = "half";
            else if (type == "torch.BFloat16Storage")
                storage->dtype = "bfloat16";
            else if (type == "torch.DoubleStorage")
                storage->dtype = "double";
            else
                return nullptr;
            storage->text = id->items[2]->text;
            storage->number = id->items[4]->number;   // its size, in elements
            // The old layout's view: (view key, offset, size) into the root storage.
            if (id->items.size() >= 6 && id->items[5]->kind == Value::kTuple &&
                id->items[5]->items.size() >= 2)
                storage->offset = id->items[5]->items[1]->number;
            return storage;
        }

        int ElementSize(const std::string& dtype) {
            return dtype == "double" ? 8 : dtype == "float" ? 4 : 2;
        }

        float ToFloat(const uint8_t* p, const std::string& dtype) {
            if (dtype == "float") {
                float f;
                memcpy(&f, p, 4);
                return f;
            }
            if (dtype == "double") {
                double d;
                memcpy(&d, p, 8);
                return static_cast<float>(d);
            }
            uint16_t h;
            memcpy(&h, p, 2);
            if (dtype == "half")
                return DirectX::PackedVector::XMConvertHalfToFloat(h);
            const uint32_t bits = static_cast<uint32_t>(h) << 16;   // bfloat16
            float f;
            memcpy(&f, &bits, 4);
            return f;
        }

        // ---- The zip PyTorch has saved in since 1.6 --------------------------------------------

        uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
        uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

        // Each stored entry's name, start and size. PyTorch stores, never compresses.
        bool ReadZip(const std::vector<uint8_t>& file,
                     std::map<std::string, std::pair<size_t, size_t>>* entries, std::string* error) {
            const size_t size = file.size();
            const uint8_t* d = file.data();
            // The end record is the last 22 bytes, or earlier by a comment of up to 64 KB.
            size_t end = std::string::npos;
            for (size_t back = 22; back <= size && back <= 22 + 65535; ++back) {
                if (Le32(d + size - back) == 0x06054b50) {
                    end = size - back;
                    break;
                }
            }
            if (end == std::string::npos) {
                *error = "the zip has no end record";
                return false;
            }
            const uint16_t count = Le16(d + end + 10);
            size_t at = Le32(d + end + 16);
            for (uint16_t n = 0; n < count; ++n) {
                if (at + 46 > size || Le32(d + at) != 0x02014b50) {
                    *error = "the zip's directory is broken";
                    return false;
                }
                const uint16_t method = Le16(d + at + 10);
                const uint32_t stored = Le32(d + at + 20);
                const uint16_t name_length = Le16(d + at + 28), extra = Le16(d + at + 30),
                               comment = Le16(d + at + 32);
                const uint32_t local = Le32(d + at + 42);
                const std::string name(reinterpret_cast<const char*>(d + at + 46), name_length);
                if (local + 30 > size || Le32(d + local) != 0x04034b50) {
                    *error = "the zip's entry " + name + " is broken";
                    return false;
                }
                const size_t start = local + 30 + Le16(d + local + 26) + Le16(d + local + 28);
                if (method != 0) {
                    *error = "the zip's entry " + name + " is compressed, which PyTorch never does";
                    return false;
                }
                if (start + stored > size) {
                    *error = "the zip's entry " + name + " runs past its end";
                    return false;
                }
                (*entries)[name] = std::make_pair(start, static_cast<size_t>(stored));
                at += 46 + name_length + extra + comment;
            }
            return true;
        }

        // The dict of tensors in what the file pickled: itself, or one held under a known key.
        Ref FindStateDict(const Ref& root) {
            if (root == nullptr || root->kind != Value::kDict)
                return nullptr;
            bool tensors = false;
            for (size_t i = 1; i < root->items.size(); i += 2)
                tensors = tensors || root->items[i]->kind == Value::kTensor;
            if (tensors)
                return root;
            for (const char* key : { "params_ema", "params", "state_dict", "model" })
                for (size_t i = 0; i + 1 < root->items.size(); i += 2)
                    if (root->items[i]->text == key)
                        if (Ref found = FindStateDict(root->items[i + 1]))
                            return found;
            return nullptr;
        }

        bool Contiguous(const Value& tensor) {
            int64_t expected = 1;
            for (size_t i = tensor.shape.size(); i-- > 0;) {
                if (tensor.shape[i] != 1 && tensor.stride[i] != expected)
                    return false;
                expected *= tensor.shape[i];
            }
            return true;
        }

        // ---- ONNX, written by hand: protobuf is varints and lengths ----------------------------

        class Proto {
         public:
            void Varint(uint64_t value) {
                while (value >= 0x80) {
                    bytes_ += static_cast<char>((value & 0x7F) | 0x80);
                    value >>= 7;
                }
                bytes_ += static_cast<char>(value);
            }
            void Key(int field, int wire) { Varint((static_cast<uint64_t>(field) << 3) | wire); }
            void Int(int field, int64_t value) {
                Key(field, 0);
                Varint(static_cast<uint64_t>(value));
            }
            void Float(int field, float value) {
                Key(field, 5);
                bytes_.append(reinterpret_cast<const char*>(&value), 4);
            }
            void Bytes(int field, const std::string& value) {
                Key(field, 2);
                Varint(value.size());
                bytes_ += value;
            }
            void Bytes(int field, const void* data, size_t size) {
                Key(field, 2);
                Varint(size);
                bytes_.append(static_cast<const char*>(data), size);
            }
            void Message(int field, const Proto& message) { Bytes(field, message.bytes_); }
            const std::string& bytes() const { return bytes_; }

         private:
            std::string bytes_;
        };

        struct Attribute {
            std::string name;
            enum { kInt = 2, kFloat = 1, kString = 3, kInts = 7 } type;
            int64_t i = 0;
            float f = 0.0f;
            std::string s;
            std::vector<int64_t> ints;
        };
        Attribute IntAttr(const char* name, int64_t value) {
            Attribute a; a.name = name; a.type = Attribute::kInt; a.i = value; return a;
        }
        Attribute FloatAttr(const char* name, float value) {
            Attribute a; a.name = name; a.type = Attribute::kFloat; a.f = value; return a;
        }
        Attribute StringAttr(const char* name, const char* value) {
            Attribute a; a.name = name; a.type = Attribute::kString; a.s = value; return a;
        }
        Attribute IntsAttr(const char* name, std::vector<int64_t> values) {
            Attribute a; a.name = name; a.type = Attribute::kInts; a.ints = std::move(values); return a;
        }

        class Graph {
         public:
            explicit Graph(const std::map<std::string, TorchTensor>& weights) : weights_(weights) {}

            std::string Fresh(const char* stem) { return std::string(stem) + "_" + std::to_string(++names_); }

            void Initializer(const std::string& name, const std::vector<int64_t>& shape,
                             const float* data, size_t count) {
                Proto tensor;
                for (int64_t d : shape)
                    tensor.Int(1, d);
                tensor.Int(2, 1);   // FLOAT
                tensor.Bytes(8, name);
                tensor.Bytes(9, data, count * sizeof(float));
                graph_.Message(5, tensor);
            }
            void Int64Initializer(const std::string& name, const std::vector<int64_t>& values) {
                Proto tensor;
                tensor.Int(1, static_cast<int64_t>(values.size()));
                tensor.Int(2, 7);   // INT64
                tensor.Bytes(8, name);
                tensor.Bytes(9, values.data(), values.size() * sizeof(int64_t));
                graph_.Message(5, tensor);
            }
            std::string Constant(const char* stem, const std::vector<int64_t>& shape,
                                 const std::vector<float>& values) {
                const std::string name = Fresh(stem);
                Initializer(name, shape, values.data(), values.size());
                return name;
            }
            std::string Node(const char* op, const std::vector<std::string>& inputs,
                             const std::vector<Attribute>& attributes = {}) {
                const std::string output = Fresh(op);
                Proto node;
                for (const std::string& in : inputs)
                    node.Bytes(1, in);
                node.Bytes(2, output);
                node.Bytes(3, output);
                node.Bytes(4, std::string(op));
                for (const Attribute& a : attributes) {
                    Proto attribute;
                    attribute.Bytes(1, a.name);
                    if (a.type == Attribute::kInt)
                        attribute.Int(3, a.i);
                    else if (a.type == Attribute::kFloat)
                        attribute.Float(2, a.f);
                    else if (a.type == Attribute::kString)
                        attribute.Bytes(4, a.s);
                    else
                        for (int64_t v : a.ints)
                            attribute.Int(8, v);
                    attribute.Int(20, a.type);
                    node.Message(5, attribute);
                }
                graph_.Message(1, node);
                return output;
            }

            // A 3x3 convolution (or any kernel the weight has), padded to keep the size, from
            // the parameters `prefix`.weight and `prefix`.bias - which become initializers once.
            bool Conv(const std::string& prefix, const std::string& input, std::string* output,
                      int64_t* out_channels, std::string* error) {
                auto weight = weights_.find(prefix + ".weight");
                auto bias = weights_.find(prefix + ".bias");
                if (weight == weights_.end() || weight->second.shape.size() != 4) {
                    *error = "no convolution " + prefix;
                    return false;
                }
                const std::vector<int64_t>& shape = weight->second.shape;
                if (used_.insert(prefix).second) {
                    Initializer(prefix + ".weight", shape, weight->second.data.data(),
                                weight->second.data.size());
                    if (bias != weights_.end())
                        Initializer(prefix + ".bias", bias->second.shape, bias->second.data.data(),
                                    bias->second.data.size());
                }
                std::vector<std::string> inputs = { input, prefix + ".weight" };
                if (bias != weights_.end())
                    inputs.push_back(prefix + ".bias");
                const int64_t pad_y = shape[2] / 2, pad_x = shape[3] / 2;
                *output = Node("Conv", inputs,
                               { IntsAttr("kernel_shape", { shape[2], shape[3] }),
                                 IntsAttr("pads", { pad_y, pad_x, pad_y, pad_x }) });
                if (out_channels != nullptr)
                    *out_channels = shape[0];
                return true;
            }

            std::string LeakyRelu(const std::string& input, float slope) {
                return Node("LeakyRelu", { input }, { FloatAttr("alpha", slope) });
            }
            std::string Upsample2(const std::string& input) {
                if (scales_.empty())
                    scales_ = Constant("scales", { 4 }, { 1.0f, 1.0f, 2.0f, 2.0f });
                return Resize(input, scales_);
            }
            std::string Resize(const std::string& input, const std::string& scales) {
                return Node("Resize", { input, "", scales },
                            { StringAttr("mode", "nearest"),
                              StringAttr("coordinate_transformation_mode", "asymmetric"),
                              StringAttr("nearest_mode", "floor") });
            }
            std::string Scale(const std::string& input, float factor) {
                auto found = factors_.find(factor);
                if (found == factors_.end())
                    found = factors_.emplace(factor, Constant("factor", { 1 }, { factor })).first;
                return Node("Mul", { input, found->second });
            }

            // The whole model, `input` to `output`, with free height and width.
            std::string Finish(const std::string& input, const std::string& output) {
                auto value_info = [](const std::string& name) {
                    Proto dims;
                    const char* params[] = { "batch", nullptr, "height", "width" };
                    for (int i = 0; i < 4; ++i) {
                        Proto dim;
                        if (params[i] != nullptr)
                            dim.Bytes(2, std::string(params[i]));
                        else
                            dim.Int(1, 3);
                        dims.Message(1, dim);
                    }
                    Proto tensor;
                    tensor.Int(1, 1);   // FLOAT
                    tensor.Message(2, dims);
                    Proto type;
                    type.Message(1, tensor);
                    Proto info;
                    info.Bytes(1, name);
                    info.Message(2, type);
                    return info;
                };
                // The output is renamed to "output" through an Identity, the input is "input".
                Proto node;
                node.Bytes(1, output);
                node.Bytes(2, std::string("output"));
                node.Bytes(3, std::string("output"));
                node.Bytes(4, std::string("Identity"));
                graph_.Message(1, node);
                graph_.Bytes(2, std::string("upscaler"));
                graph_.Message(11, value_info(input));
                graph_.Message(12, value_info("output"));

                Proto opset;
                opset.Bytes(1, std::string());
                opset.Int(2, 17);
                Proto model;
                model.Int(1, 8);   // IR version 8, which opset 17 goes with
                model.Bytes(2, std::string("psxemu"));
                model.Message(7, graph_);
                model.Message(8, opset);
                return model.bytes();
            }

         private:
            const std::map<std::string, TorchTensor>& weights_;
            Proto graph_;
            int names_ = 0;
            std::set<std::string> used_;
            std::string scales_;
            std::map<float, std::string> factors_;
        };

        bool Has(const std::map<std::string, TorchTensor>& weights, const std::string& name) {
            return weights.count(name) != 0;
        }

        // ESRGAN's residual-in-residual dense block, under either naming: `rdb` is the dense
        // block's prefix and `conv` how its k-th convolution is named within it.
        bool DenseBlock(Graph& graph, const std::string& input, const std::string& rdb,
                        const std::function<std::string(int)>& conv, std::string* output,
                        std::string* error) {
            std::vector<std::string> features = { input };
            std::string x;
            for (int k = 1; k <= 5; ++k) {
                const std::string joined =
                    features.size() == 1 ? features[0] : graph.Node("Concat", features, { IntAttr("axis", 1) });
                if (!graph.Conv(rdb + conv(k), joined, &x, nullptr, error))
                    return false;
                if (k < 5) {
                    x = graph.LeakyRelu(x, 0.2f);
                    features.push_back(x);
                }
            }
            *output = graph.Node("Add", { graph.Scale(x, 0.2f), input });
            return true;
        }

        bool ExportEsrgan(const std::map<std::string, TorchTensor>& w, Graph& graph,
                          UpscalerArchitecture* architecture, std::string* output, std::string* error) {
            const bool old_names = Has(w, "model.0.weight");
            std::string first, trunk, high, last;
            std::vector<std::string> ups;
            std::function<std::string(int, int)> rdb;
            std::function<std::string(int)> conv;
            int blocks = 0;
            if (old_names) {
                // model.0, model.1.sub.0..nb-1 then model.1.sub.nb (the trunk's last), then an
                // upsampling convolution every three places from model.3, then HR and last.
                first = "model.0";
                while (Has(w, "model.1.sub." + std::to_string(blocks) + ".RDB1.conv1.0.weight"))
                    ++blocks;
                trunk = "model.1.sub." + std::to_string(blocks);
                std::vector<int> tops;
                for (int i = 2; i < 64; ++i)
                    if (Has(w, "model." + std::to_string(i) + ".weight"))
                        tops.push_back(i);
                if (tops.size() < 2) {
                    *error = "an ESRGAN with no HR and last convolutions";
                    return false;
                }
                for (size_t i = 0; i + 2 < tops.size(); ++i)
                    ups.push_back("model." + std::to_string(tops[i]));
                high = "model." + std::to_string(tops[tops.size() - 2]);
                last = "model." + std::to_string(tops.back());
                rdb = [](int b, int r) {
                    return "model.1.sub." + std::to_string(b) + ".RDB" + std::to_string(r);
                };
                conv = [](int k) { return ".conv" + std::to_string(k) + ".0"; };
            } else {
                first = "conv_first";
                while (Has(w, "body." + std::to_string(blocks) + ".rdb1.conv1.weight"))
                    ++blocks;
                trunk = "conv_body";
                for (const char* up : { "conv_up1", "conv_up2", "conv_up3" })
                    if (Has(w, std::string(up) + ".weight"))
                        ups.push_back(up);
                high = "conv_hr";
                last = "conv_last";
                rdb = [](int b, int r) {
                    return "body." + std::to_string(b) + ".rdb" + std::to_string(r);
                };
                conv = [](int k) { return ".conv" + std::to_string(k); };
            }
            if (blocks == 0) {
                *error = "an ESRGAN with no RRDBs";
                return false;
            }
            const TorchTensor& first_weight = w.at(first + ".weight");
            const int64_t in_channels = first_weight.shape[1];
            // BasicSR's 2x and 1x ESRGANs take the picture pixel-unshuffled - 12 or 48 channels -
            // and upsample 4x after. ONNX's SpaceToDepth orders the channels block-major, PyTorch's
            // pixel_unshuffle channel-major: a Gather puts them in PyTorch's order.
            int unshuffle = in_channels == 12 ? 2 : in_channels == 48 ? 4 : 1;
            if (in_channels != 3 && unshuffle == 1) {
                *error = "an ESRGAN taking " + std::to_string(in_channels) + " channels";
                return false;
            }
            std::string x = "input";
            if (unshuffle > 1) {
                x = graph.Node("SpaceToDepth", { x }, { IntAttr("blocksize", unshuffle) });
                std::vector<int64_t> order;
                for (int c = 0; c < 3; ++c)
                    for (int i = 0; i < unshuffle * unshuffle; ++i)
                        order.push_back(static_cast<int64_t>(i) * 3 + c);
                const std::string indices = graph.Fresh("order");
                graph.Int64Initializer(indices, order);
                x = graph.Node("Gather", { x, indices }, { IntAttr("axis", 1) });
            }
            std::string features;
            int64_t nf = 0;
            if (!graph.Conv(first, x, &features, &nf, error))
                return false;
            std::string body = features;
            for (int b = 0; b < blocks; ++b) {
                std::string y = body;
                for (int r = 1; r <= 3; ++r)
                    if (!DenseBlock(graph, y, rdb(b, r), conv, &y, error))
                        return false;
                body = graph.Node("Add", { graph.Scale(y, 0.2f), body });
            }
            if (!graph.Conv(trunk, body, &body, nullptr, error))
                return false;
            x = graph.Node("Add", { features, body });
            for (const std::string& up : ups) {
                if (!graph.Conv(up, graph.Upsample2(x), &x, nullptr, error))
                    return false;
                x = graph.LeakyRelu(x, 0.2f);
            }
            if (!graph.Conv(high, x, &x, nullptr, error))
                return false;
            int64_t out_channels = 0;
            if (!graph.Conv(last, graph.LeakyRelu(x, 0.2f), &x, &out_channels, error))
                return false;
            const int scale = (1 << ups.size()) / unshuffle;
            if (out_channels != 3 || scale < 1) {
                *error = "an ESRGAN giving " + std::to_string(out_channels) + " channels";
                return false;
            }
            architecture->name = "esrgan";
            architecture->scale = scale;
            architecture->features = static_cast<int>(nf);
            architecture->blocks = blocks;
            architecture->in_channels = 3;
            architecture->out_channels = 3;
            *output = x;
            return true;
        }

        bool ExportCompact(const std::map<std::string, TorchTensor>& w, Graph& graph,
                           UpscalerArchitecture* architecture, std::string* output, std::string* error) {
            // body.0 conv, body.1 activation, body.2 conv... the last conv gives 3 x scale^2
            // channels, pixel-shuffled; the input, upsampled nearest, is added to it.
            std::vector<int> convs;
            for (int i = 0; i < 256; ++i) {
                auto found = w.find("body." + std::to_string(i) + ".weight");
                if (found != w.end() && found->second.shape.size() == 4)
                    convs.push_back(i);
            }
            if (convs.size() < 2) {
                *error = "a compact model with fewer than two convolutions";
                return false;
            }
            const bool prelu = Has(w, "body.1.weight");
            std::string x = "input";
            int64_t channels = 0, nf = 0;
            for (size_t n = 0; n < convs.size(); ++n) {
                if (!graph.Conv("body." + std::to_string(convs[n]), x, &x, &channels, error))
                    return false;
                if (n == 0)
                    nf = channels;
                if (n + 1 == convs.size())
                    break;
                const std::string act = "body." + std::to_string(convs[n] + 1) + ".weight";
                if (prelu) {
                    auto slope = w.find(act);
                    if (slope == w.end()) {
                        *error = "a compact model missing its PReLU " + act;
                        return false;
                    }
                    const std::string name = graph.Constant(
                        "slope", { static_cast<int64_t>(slope->second.data.size()), 1, 1 },
                        slope->second.data);
                    x = graph.Node("PRelu", { x, name });
                } else {
                    x = graph.LeakyRelu(x, 0.1f);
                }
            }
            const int scale = static_cast<int>(std::lround(std::sqrt(static_cast<double>(channels) / 3.0)));
            if (scale < 1 || scale * scale * 3 != channels) {
                *error = "a compact model ending in " + std::to_string(channels) + " channels";
                return false;
            }
            x = graph.Node("DepthToSpace", { x }, { IntAttr("blocksize", scale), StringAttr("mode", "CRD") });
            const float s = static_cast<float>(scale);
            const std::string scales = graph.Constant("scales", { 4 }, { 1.0f, 1.0f, s, s });
            x = graph.Node("Add", { x, graph.Resize("input", scales) });
            architecture->name = "compact";
            architecture->scale = scale;
            architecture->features = static_cast<int>(nf);
            architecture->blocks = static_cast<int>(convs.size()) - 2;
            architecture->in_channels = 3;
            architecture->out_channels = 3;
            *output = x;
            return true;
        }

    }   // namespace

    bool ReadTorchStateDict(const std::wstring& path, std::map<std::string, TorchTensor>* weights,
                            std::string* error) {
        std::vector<uint8_t> file;
        {
            FILE* f = _wfopen(path.c_str(), L"rb");
            if (f == nullptr) {
                *error = "the file could not be opened";
                return false;
            }
            fseek(f, 0, SEEK_END);
            const long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            file.resize(size > 0 ? static_cast<size_t>(size) : 0);
            const size_t got = file.empty() ? 0 : fread(file.data(), 1, file.size(), f);
            fclose(f);
            if (got != file.size() || file.size() < 8) {
                *error = "the file could not be read";
                return false;
            }
        }

        Ref root;
        // Where each storage's bytes are, by key.
        std::map<std::string, std::pair<size_t, size_t>> storages;
        if (file[0] == 'P' && file[1] == 'K') {
            std::map<std::string, std::pair<size_t, size_t>> entries;
            if (!ReadZip(file, &entries, error))
                return false;
            std::string folder;
            for (const auto& entry : entries) {
                const size_t slash = entry.first.rfind('/');
                if (slash != std::string::npos && entry.first.substr(slash + 1) == "data.pkl")
                    folder = entry.first.substr(0, slash + 1);
            }
            auto pickle = entries.find(folder + "data.pkl");
            if (pickle == entries.end()) {
                *error = "the zip has no data.pkl";
                return false;
            }
            for (const auto& entry : entries)
                if (entry.first.rfind(folder + "data/", 0) == 0)
                    storages[entry.first.substr(folder.size() + 5)] = entry.second;
            Unpickler unpickler(file.data() + pickle->second.first, pickle->second.second, Storage);
            root = unpickler.Load(error);
            if (root == nullptr)
                return false;
        } else {
            // The old layout: pickles of a magic number, the protocol, the system's sizes, the
            // object, and the list of storage keys; then each storage, in that list's order, as
            // its element count and its bytes.
            Unpickler unpickler(file.data(), file.size(), Storage);
            // The magic number is 0x1950a86a20f9469cfc6c, ten bytes, of which the integer kept
            // here holds the low eight.
            Ref magic = unpickler.Load(error);
            if (magic == nullptr)
                return false;
            if (magic->kind != Value::kInt || static_cast<uint64_t>(magic->number) != 0xa86a20f9469cfc6cULL) {
                *error = "the file is neither a zip nor PyTorch's old layout";
                return false;
            }
            if (unpickler.Load(error) == nullptr || unpickler.Load(error) == nullptr)
                return false;
            root = unpickler.Load(error);
            Ref keys = root != nullptr ? unpickler.Load(error) : nullptr;
            if (root == nullptr || keys == nullptr)
                return false;
            // The element sizes, from the storages the object named.
            std::map<std::string, int> sizes;
            std::function<void(const Ref&)> walk = [&](const Ref& value) {
                if (value->kind == Value::kTensor)
                    sizes[value->storage->text] = ElementSize(value->storage->dtype);
                for (const Ref& item : value->items)
                    walk(item);
            };
            walk(root);
            size_t at = unpickler.position();
            for (const Ref& key : keys->items) {
                if (at + 8 > file.size()) {
                    *error = "the file ends before its storages do";
                    return false;
                }
                uint64_t count = 0;
                for (int i = 0; i < 8; ++i)
                    count |= static_cast<uint64_t>(file[at + i]) << (8 * i);
                at += 8;
                const int element = sizes.count(key->text) != 0 ? sizes[key->text] : 4;
                const size_t bytes = static_cast<size_t>(count) * element;
                if (at + bytes > file.size()) {
                    *error = "the file ends inside a storage";
                    return false;
                }
                storages[key->text] = std::make_pair(at, bytes);
                at += bytes;
            }
        }

        Ref dict = FindStateDict(root);
        if (dict == nullptr) {
            *error = "the file holds no dict of tensors";
            return false;
        }
        weights->clear();
        for (size_t i = 0; i + 1 < dict->items.size(); i += 2) {
            const Value& tensor = *dict->items[i + 1];
            if (tensor.kind != Value::kTensor)
                continue;
            const std::string& name = dict->items[i]->text;
            auto storage = storages.find(tensor.storage->text);
            if (storage == storages.end()) {
                *error = "the tensor " + name + " has no storage in the file";
                return false;
            }
            if (!Contiguous(tensor)) {
                *error = "the tensor " + name + " is not contiguous";
                return false;
            }
            int64_t count = 1;
            for (int64_t d : tensor.shape)
                count *= d;
            const int element = ElementSize(tensor.storage->dtype);
            const size_t start = static_cast<size_t>(tensor.offset) * element;
            if (start + static_cast<size_t>(count) * element > storage->second.second) {
                *error = "the tensor " + name + " runs past its storage";
                return false;
            }
            TorchTensor out;
            out.shape = tensor.shape;
            out.data.resize(static_cast<size_t>(count));
            const uint8_t* p = file.data() + storage->second.first + start;
            for (int64_t n = 0; n < count; ++n)
                out.data[static_cast<size_t>(n)] = ToFloat(p + n * element, tensor.storage->dtype);
            (*weights)[name] = std::move(out);
        }
        if (weights->empty()) {
            *error = "the file's dict holds no tensors";
            return false;
        }
        return true;
    }

    bool ExportUpscalerOnnx(const std::map<std::string, TorchTensor>& weights, std::string* onnx,
                            UpscalerArchitecture* architecture, std::string* error) {
        Graph graph(weights);
        std::string output;
        bool done = false;
        if (Has(weights, "model.0.weight") || Has(weights, "conv_first.weight"))
            done = ExportEsrgan(weights, graph, architecture, &output, error);
        else if (Has(weights, "body.0.weight"))
            done = ExportCompact(weights, graph, architecture, &output, error);
        else
            *error = "the weights are neither an ESRGAN nor a compact model";
        if (!done)
            return false;
        *onnx = graph.Finish("input", output);
        return true;
    }

}   // namespace emulation::win32
