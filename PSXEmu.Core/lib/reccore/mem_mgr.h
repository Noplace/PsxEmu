#pragma once


class MemMgr {
public:
	MemMgr(size_t size) : size(size) {
		base_address = VirtualAlloc(0, size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	}
	virtual ~MemMgr() {
		VirtualFree(base_address, size, MEM_DECOMMIT | MEM_RELEASE);
	}

	void* alloc(size_t size) {
		uint8_t* newblock = static_cast<uint8_t*>(base_address) + offset;
		offset += size;
		return newblock;
	}
private:
	size_t size;
	void* base_address;
	size_t offset;
};