// Copyright 2020 Western Digital Corporation or its affiliates.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <unordered_map>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <type_traits>
#include <cassert>
#include <PmaManager.hpp>


namespace ELFIO
{
  class elfio;
}


namespace WdRiscv
{

  template <typename URV>
  class Hart;

  /// Location and size of an ELF file symbol.
  struct ElfSymbol
  {
    ElfSymbol(size_t addr = 0, size_t size = 0)
      : addr_(addr), size_(size)
    { }

    size_t addr_ = 0;
    size_t size_ = 0;
  };


  /// Model physical memory of system.
  class Memory
  {
  public:

    friend class Hart<uint32_t>;
    friend class Hart<uint64_t>;

    /// Constructor: define a memory of the given size initialized to
    /// zero. Given memory size (byte count) must be a multiple of 4
    /// otherwise, it is truncated to a multiple of 4. The memory
    /// is partitioned into regions according to the region size which
    /// must be a power of 2.
    Memory(size_t size, size_t pageSize = 4*1024,
	   size_t regionSize = 256*1024*1024);

    /// Destructor.
    ~Memory();

    /// Define number of hardware threads for LR/SC. FIX: put this in
    /// constructor.
    void setHartCount(unsigned count)
    { reservations_.resize(count); lastWriteData_.resize(count); }

    /// Return memory size in bytes.
    size_t size() const
    { return size_; }

    /// Read an unsigned integer value of type T from memory at the
    /// given address into value. Return true on success. Return false
    /// if any of the requested bytes is out of memory bounds or fall
    /// in unmapped memory or if the read crosses memory regions of
    /// different attributes.
    template <typename T>
    bool read(size_t address, T& value) const
    {
#ifdef FAST_SLOPPY
      if (address + sizeof(T) > size_)
        return false;
#else
      Pma pma1 = pmaMgr_.getPma(address);
      if (not pma1.isRead())
	return false;

      if (address & (sizeof(T) - 1))  // If address is misaligned
	{
          Pma pma2 = pmaMgr_.getPma(address + sizeof(T) - 1);
          if (not pma2.isRead())
            return false;
        }

      // Memory mapped region accessible only with word-size read.
      if (pma1.isMemMappedReg())
        {
          if constexpr (sizeof(T) == 4)
            return readRegister(address, value);
          else
            return false;
        }
#endif
      value = *(reinterpret_cast<const T*>(data_ + address));
      return true;
    }

    /// Read byte from given address into value. Return true on
    /// success.  Return false if address is out of bounds.
    bool readByte(size_t address, uint8_t& value) const
    {
#ifdef FAST_SLOPPY
      if (address >= size_)
        return false;
#else
      Pma pma = pmaMgr_.getPma(address);
      if (not pma.isRead())
	return false;

      if (pma.isMemMappedReg())
	return false; // Only word access allowed to memory mapped regs.
#endif

      value = data_[address];
      return true;
    }

    /// Read half-word (2 bytes) from given address into value. See
    /// read method.
    bool readHalfWord(size_t address, uint16_t& value) const
    { return read(address, value); }

    /// Read word (4 bytes) from given address into value. See read
    /// method.
    bool readWord(size_t address, uint32_t& value) const
    { return read(address, value); }

    /// Read a double-word (8 bytes) from given address into
    /// value. See read method.
    bool readDoubleWord(size_t address, uint64_t& value) const
    { return read(address, value); }

    /// On a unified memory model, this is the same as readHalfWord.
    /// On a split memory model, this will taken an exception if the
    /// target address is not in instruction memory.
    bool readInstHalfWord(size_t address, uint16_t& value) const
    {
      Pma pma = pmaMgr_.getPma(address);
      if (pma.isExec())
	{
	  if (address & 1)
	    {
              // Misaligned address: Check next address.
              Pma pma2 = pmaMgr_.getPma(address + 1);
	      if (pma != pma2)
                return false;  // Cannot cross an ICCM boundary.
	    }

	  value = *(reinterpret_cast<const uint16_t*>(data_ + address));
	  return true;
	}
      return false;
    }

    /// On a unified memory model, this is the same as readWord.
    /// On a split memory model, this will taken an exception if the
    /// target address is not in instruction memory.
    bool readInstWord(size_t address, uint32_t& value) const
    {
      Pma pma = pmaMgr_.getPma(address);
      if (pma.isExec())
	{
	  if (address & 3)
	    {

	      Pma pma2 = pmaMgr_.getPma(address + 3);
	      if (pma != pma2)
                return false;
	    }

	  value = *(reinterpret_cast<const uint32_t*>(data_ + address));
	  return true;
	}
	return false;
    }

    /// Return true if write will be successful if tried. Do not
    /// write.  Change value to the maksed value if write is to a
    /// memory mapped register.
    template <typename T>
    bool checkWrite(size_t address, T& value)
    {
      Pma pma1 = pmaMgr_.getPma(address);
      if (not pma1.isWrite())
	return false;

      if (address & (sizeof(T) - 1))  // If address is misaligned
	{
          Pma pma2 = pmaMgr_.getPma(address + sizeof(T) - 1);
          if (pma1 != pma2)
            return false;
	}

      // Memory mapped region accessible only with word-size write.
      if (pma1.isMemMappedReg())
        {
          if constexpr (sizeof(T) != 4)
            return false;
          if ((address & 3) != 0)
            return false;
          value = doRegisterMasking(address, value);
        }

      return true;
    }

    /// Write given unsigned integer value of type T into memory
    /// starting at the given address. Return true on success. Return
    /// false if any of the target memory bytes are out of bounds or
    /// fall in inaccessible regions or if the write crosses memory
    /// region of different attributes.
    template <typename T>
    bool write(unsigned sysHartIx, size_t address, T value)
    {
#ifdef FAST_SLOPPY
      if (address + sizeof(T) > size_)
        return false;
      sysHartIx = sysHartIx; // Avoid unused var warning.
#else
      Pma pma1 = pmaMgr_.getPma(address);
      if (not pma1.isWrite())
	return false;

      if (address & (sizeof(T) - 1))  // If address is misaligned
	{
          Pma pma2 = pmaMgr_.getPma(address + sizeof(T) - 1);
          if (pma1 != pma2)
            return false;
	}

      // Memory mapped region accessible only with word-size write.
      if (pma1.isMemMappedReg())
        {
          if constexpr (sizeof(T) != 4)
            return false;
          return writeRegister(sysHartIx, address, value);
	}

      auto& lwd = lastWriteData_.at(sysHartIx);
      lwd.prevValue_ = *(reinterpret_cast<T*>(data_ + address));
      lwd.size_ = sizeof(T);
      lwd.addr_ = address;
      lwd.value_ = value;
#endif

      *(reinterpret_cast<T*>(data_ + address)) = value;
      return true;
    }

    /// Write byte to given address. Return true on success. Return
    /// false if address is out of bounds or is not writable.
    bool writeByte(unsigned sysHartIx, size_t address, uint8_t value)
    {
#ifdef FAST_SLOPPY
      if (address >= size_)
        return false;
      sysHartIx = sysHartIx; // Avoid unused var warning.
#else
      Pma pma = pmaMgr_.getPma(address);
      if (not pma.isWrite())
	return false;

      if (pma.isMemMappedReg())
	return false;  // Only word access allowed to memory mapped regs.

      auto& lwd = lastWriteData_.at(sysHartIx);
      lwd.prevValue_ = *(data_ + address);
      lwd.size_ = 1;
      lwd.addr_ = address;
      lwd.value_ = value;
#endif

      data_[address] = value;
      return true;
    }

    /// Write half-word (2 bytes) to given address. Return true on
    /// success. Return false if address is out of bounds or is not
    /// writable.
    bool writeHalfWord(unsigned sysHartIx, size_t address, uint16_t value)
    { return write(sysHartIx, address, value); }

    /// Read word (4 bytes) from given address into value. Return true
    /// on success.  Return false if address is out of bounds or is
    /// not writable.
    bool writeWord(unsigned sysHartIx, size_t address, uint32_t value)
    { return write(sysHartIx, address, value); }

    /// Read a double-word (8 bytes) from given address into
    /// value. Return true on success. Return false if address is out
    /// of bounds.
    bool writeDoubleWord(unsigned sysHartIx, size_t address, uint64_t value)
    { return write(sysHartIx, address, value); }

    /// Load the given hex file and set memory locations accordingly.
    /// Return true on success. Return false if file does not exists,
    /// cannot be opened or contains malformed data.
    /// File format: A line either contains @address where address
    /// is a hexadecimal memory address or one or more space separated
    /// tokens each consisting of two hexadecimal digits.
    bool loadHexFile(const std::string& file);

    /// Load the given ELF file and set memory locations accordingly.
    /// Return true on success. Return false if file does not exists,
    /// cannot be opened or contains malformed data, or if it contains
    /// data incompatible with the given register width (32 or 64). If
    /// successful, set entryPoint to the entry point of the loaded
    /// file and end to the address past that of the loaded byte with
    /// the largest address. Extract symbol names and corresponding
    /// addresses and sizes into the memory symbols map.
    bool loadElfFile(const std::string& file, unsigned registerWidth,
		     size_t& entryPoint, size_t& end);

    /// Locate the given ELF symbol (symbols are collected for every
    /// loaded ELF file) returning true if symbol is found and false
    /// otherwise. Set value to the corresponding value if symbol is
    /// found.
    bool findElfSymbol(const std::string& symbol, ElfSymbol& value) const;

    /// Locate the ELF function cotaining the give address returning true
    /// on success and false on failure.  If successful set name to the
    /// corresponding function name and symbol to the corresponding symbol
    /// value.
    bool findElfFunction(size_t addr, std::string& name, ElfSymbol& value) const;

    /// Print the ELF symbols on the given stream. Output format:
    /// <name> <value>
    void printElfSymbols(std::ostream& out) const;

    /// Enable/disable errors on unmapped memory when loading ELF files.
    void checkUnmappedElf(bool flag)
    { checkUnmappedElf_ = flag; }

    /// Return the min and max addresses corresponding to the segments
    /// in the given ELF file. Return true on success and false if
    /// the ELF file does not exist or cannot be read (in which
    /// case min and max address are left unmodified).
    static bool getElfFileAddressBounds(const std::string& file,
					size_t& minAddr, size_t& maxAddr);

    /// Copy data from the given memory into this memory. If the two
    /// memories have different sizes then copy data from location
    /// zero up to n-1 where n is the minimum of the sizes.
    void copy(const Memory& other);

    /// Return true if given path corresponds to an ELF file and set
    /// the given flags according to the contents of the file.  Return
    /// false leaving the flags unmodified if file does not exist,
    /// cannot be read, or is not an ELF file.
    static bool checkElfFile(const std::string& path, bool& is32bit,
			     bool& is64bit, bool& isRiscv);

    /// Return true if given symbol is present in the given ELF file.
    static bool isSymbolInElfFile(const std::string& path,
				  const std::string& target);

  protected:

    /// Same as write but effects not recorded in last-write info.
    template <typename T>
    bool poke(size_t address, T value)
    {
      Pma pma1 = pmaMgr_.getPma(address);
      if (not pma1.isMapped())
	return false;

      if (address & (sizeof(T) - 1))  // If address is misaligned
	{
          Pma pma2 = pmaMgr_.getPma(address + sizeof(T) - 1);
          if (not pma2.isMapped())
	    return false;
	}

      // Memory mapped region accessible only with word-size poke.
      if (pma1.isMemMappedReg())
        {
          if constexpr (sizeof(T) != 4)
            return false;
          return pmaMgr_.writeRegisterNoMask(address, value);
        }

      *(reinterpret_cast<T*>(data_ + address)) = value;
      return true;
    }

    /// Same as writeByte but effects are not record in last-write info.
    bool pokeByte(size_t address, uint8_t value)
    {
      Pma pma = pmaMgr_.getPma(address);
      if (not pma.isMapped())
	return false;

      if (pma.isMemMappedReg())
	return false;  // Only word access allowed to memory mapped regs.

      data_[address] = value;
      return true;
    }

    /// Write byte to given address without write-access check. Return
    /// true on success. Return false if address is not mapped. This
    /// is used to initialize memory. If address is in
    /// memory-mapped-register region, then both mem-mapped-register
    /// and external memory are written.
    bool specialInitializeByte(size_t address, uint8_t value);

    /// Set addr to the address of the last write and value to the
    /// corresponding value and return the size of that write. Return
    /// 0 if no write since the most recent clearLastWriteInfo in
    /// which case addr and value are not modified.
    unsigned getLastWriteNewValue(unsigned sysHartIx, size_t& addr, uint64_t& value) const
    {
      const auto& lwd = lastWriteData_.at(sysHartIx);
      if (lwd.size_)
	{
	  addr = lwd.addr_;
	  value = lwd.value_;
	}
      return lwd.size_;
    }

    /// Set addr to the address of the last write and value to the
    /// corresponding previous value (value before last write) and
    /// return the size of that write. Return 0 if no write since the
    /// most recent clearLastWriteInfo in which case addr and value
    /// are not modified.
    unsigned getLastWriteOldValue(unsigned sysHartIx, size_t& addr,
                                  uint64_t& value) const
    {
      auto& lwd = lastWriteData_.at(sysHartIx);
      if (lwd.size_)
	{
	  addr = lwd.addr_;
	  value = lwd.value_;
	}
      return lwd.size_;
    }

    /// Clear the information associated with last write.
    void clearLastWriteInfo(unsigned sysHartIx)
    {
      auto& lwd = lastWriteData_.at(sysHartIx);
      lwd.size_ = 0;
    }

    /// Return the page size.
    size_t pageSize() const
    { return pageSize_; }

    /// Return the region size.
    size_t regionSize() const
    { return regionSize_; }

    /// Return the number of the page containing the given address.
    size_t getPageIx(size_t addr) const
    { return addr >> pageShift_; }

    /// Return start address of page containing given address.
    size_t getPageStartAddr(size_t addr) const
    { return (addr >> pageShift_) << pageShift_; }

    /// Return true if CCM (iccm or dccm) configuration defined by
    /// addr/size is valid. Return false otherwise. Tag parameter
    /// ("iccm"/"dccm"/"pic") is used with error messages.
    bool checkCcmConfig(const std::string& tag, size_t addr, size_t size) const;

    /// Complain if CCM (iccm or dccm) defined by region/offset/size
    /// overlaps a previously defined CCM area. Return true if all is
    /// well (no overlap).
    bool checkCcmOverlap(const std::string& tag, size_t addr, size_t size,
			 bool iccm, bool dccm, bool pic) const;

    /// If a region contains dccm/pic or iccm (but not both) then only
    /// the proper dcc/pic or iccm area is accessible.
    void narrowCcmRegion(size_t addr, bool trim);

    /// Define instruction closed coupled memory (in core instruction memory).
    /// If trim is true then region containing ICCM is marked inaccessible
    /// except for the ICCM area.
    bool defineIccm(size_t addr, size_t size, bool trim);

    /// Define data closed coupled memory (in core data memory). If
    /// trim is true then region containing DCCM is marked
    /// inaccessible except for the ICCM area.
    bool defineDccm(size_t addr, size_t size, bool trim);

    /// Define region for memory mapped registers (MMR). Return true
    /// on success and false if offset or size are not properly
    /// aligned or sized. If trim is true then region containing MMR
    /// is marked inaccessible except for the MMR area.
    bool defineMemoryMappedRegisterArea(size_t addr, size_t size, bool trim );

    /// Reset (to zero) all memory mapped registers.
    void resetMemoryMappedRegisters();

    /// Define write mask for a memory-mapped register with given
    /// address.  Return true on success and false if the address is
    /// not within a memory-mapped area (see
    /// defineMemoryMappedRegisterArea).
    bool defineMemoryMappedRegisterWriteMask(size_t addr, uint32_t mask);

    /// Called after memory is configured to refine memory access to
    /// sections of regions containing ICCM, DCCM or memory mapped
    /// register area (e.g. PIC).
    void finishCcmConfig(bool iccmRw);

    /// Read a memory mapped register.
    bool readRegister(size_t addr, uint32_t& value) const
    {
      return pmaMgr_.readRegister(addr, value);
    }

    /// Return memory mapped mask associated with the word containing
    /// the given address. Return all 1 if given address is not a
    /// memory mapped register.
    uint32_t getMemoryMappedMask(size_t addr) const
    { return pmaMgr_.getMemMappedMask(addr); }

    /// Perform masking for a write to a memory mapped register.
    /// Return masked value.
    uint32_t doRegisterMasking(size_t addr, uint32_t value) const
    {
      uint32_t mask = getMemoryMappedMask(addr);
      value = value & mask;
      return value;
    }

    /// Write a memory mapped register.
    bool writeRegister(unsigned sysHartIx, size_t addr, uint32_t value)
    {
      uint32_t prev = 0;
      if (not readRegister(addr, prev))
        return false;

      value = doRegisterMasking(addr, value);

      if (not pmaMgr_.writeRegister(addr, value))
        return false;

      auto& lwd = lastWriteData_.at(sysHartIx);
      lwd.prevValue_ = prev;
      lwd.size_ = 4;
      lwd.addr_ = addr;
      lwd.value_ = value;
      return true;
    }

    /// Return the number of the 256-mb region containing given address.
    size_t getRegionIndex(size_t addr) const
    { return (addr >> regionShift_) & regionMask_; }

    /// Return true if given data address is external to the core.
    bool isDataAddressExternal(size_t addr) const
    {
      Pma pma = pmaMgr_.getPma(addr);
      return not (pma.isDccm() or pma.isMemMappedReg());
    }

    /// Return the simulator memory address corresponding to the
    /// simulated RISCV memory address. This is useful for Linux
    /// emulation.
    bool getSimMemAddr(size_t addr, size_t& simAddr)
    {
      if (addr >= size_)
	return false;
      simAddr = reinterpret_cast<size_t>(data_ + addr);
      return true;
    }

    /// Track LR instructin resrvations.
    struct Reservation
    {
      size_t addr_ = 0;
      unsigned size_ = 0;
      bool valid_ = false;
    };
      
    /// Invalidate LR reservations matching address of poked/written
    /// bytes and belonging to harts other than the given hart-id. The
    /// memory tracks one reservation per hart indexed by local hart
    /// ids.
    void invalidateOtherHartLr(unsigned sysHartIx, size_t addr,
                               unsigned storeSize)
    {
      for (size_t i = 0; i < reservations_.size(); ++i)
        {
          if (i == sysHartIx) continue;
          auto& res = reservations_[i];
          if (addr >= res.addr_ and (addr - res.addr_) < res.size_)
            res.valid_ = false;
          else if (addr < res.addr_ and (res.addr_ - addr) < storeSize)
            res.valid_ = false;
        }
    }

    /// Invalidate LR reservations matching address of poked/written
    /// bytes. The memory tracks one reservation per hart indexed by
    /// local hart ids.
    void invalidateLrs(size_t addr, unsigned storeSize)
    {
      for (size_t i = 0; i < reservations_.size(); ++i)
        {
          auto& res = reservations_[i];
          if (addr >= res.addr_ and (addr - res.addr_) < res.size_)
            res.valid_ = false;
          else if (addr < res.addr_ and (res.addr_ - addr) < storeSize)
            res.valid_ = false;
        }
    }

    /// Invalidate LR reservation corresponding to the given hart.
    void invalidateLr(unsigned sysHartIx)
    { reservations_.at(sysHartIx).valid_ = false; }

    /// Make a LR reservation for the given hart.
    void makeLr(unsigned sysHartIx, size_t addr, unsigned size)
    {
      auto& res = reservations_.at(sysHartIx);
      res.addr_ = addr;
      res.size_ = size;
      res.valid_ = true;
    }

    /// Return true if given hart has a valid LR reservation for the
    /// given address.
    bool hasLr(unsigned sysHartIx, size_t addr) const
    {
      auto& res = reservations_.at(sysHartIx);
      return res.valid_ and res.addr_ == addr;
    }

    /// Load contents of given ELF segment into memory.
    /// This is a helper to loadElfFile.
    bool loadElfSegment(ELFIO::elfio& reader, int segment, size_t& end,
                        size_t& overwrites);

    /// Take a snapshot of the entire simulated memory into binary
    /// file. Return true on success or false on failure
    bool saveSnapshot(const std::string& filename,
                      const std::vector<std::pair<uint64_t,uint64_t>>& used_blocks);

    /// Load the simulated memory from snapshot binary file. Return
    /// true on success or false on failure
    bool loadSnapshot(const std::string& filename,
                      const std::vector<std::pair<uint64_t,uint64_t>>& used_blocks);

  private:

    /// Information about last write operation by a hart.
    struct LastWriteData
    {
      unsigned size_ = 0;
      size_t addr_ = 0;
      uint64_t value_ = 0;
      uint64_t prevValue_ = 0;
    };

    size_t size_;        // Size of memory in bytes.
    uint8_t* data_;      // Pointer to memory data.

    // Memory is organized in regions (e.g. 256 Mb). Each region is
    // organized in pages (e.g 4kb). Each page is associated with
    // access attributes. Memory mapped register pages are also
    // associated with write-masks (one 4-byte mask per word).
    size_t regionCount_    = 16;
    size_t regionSize_     = 256*1024*1024;
    std::vector<bool> regionConfigured_; // One per region.
    std::vector<bool> regionHasLocalInst_; // One per region.
    std::vector<bool> regionHasLocalData_; // One per region.

    size_t pageCount_     = 1024*1024; // Should be derived from page size.
    size_t pageSize_      = 4*1024;    // Must be a power of 2.
    unsigned pageShift_   = 12;        // Shift address by this to get page no.
    unsigned regionShift_ = 28;        // Shift address by this to get region no.
    unsigned regionMask_  = 0xf;       // This should depend on mem size.

    std::mutex amoMutex_;
    std::mutex lrMutex_;

    bool checkUnmappedElf_ = true;

    std::unordered_map<std::string, ElfSymbol> symbols_;

    std::vector<Reservation> reservations_;
    std::vector<LastWriteData> lastWriteData_;

    PmaManager pmaMgr_;
  };
}
