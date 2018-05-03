#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <stdlib.h>
#include <sys/mman.h>
#include <elfio/elfio.hpp>
#include "Memory.hpp"

using namespace WdRiscv;


Memory::Memory(size_t size)
  : size_(size), data_(nullptr), attribs_(nullptr)
{ 
  if ((size & 4) != 0)
    {
      size_ = (size >> 2) << 2;
      std::cerr << "Memory size (" << size << ") is not a multiple of 4. Using "
		<< size_ << '\n';
    }

  void* mem = mmap(nullptr, size_, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (not mem)
    {
      std::cerr << "Failed to map " << size_ << " bytes using mmap.\n";
      throw std::runtime_error("Out of memory");
    }

  data_ = reinterpret_cast<uint8_t*>(mem);

  attribs_ = new uint8_t[chunkCount_];

  // Make whole memory (4 gigs) mapped, writeable, allowing data and inst.
  unsigned nirvana = SizeMask | MappedMask | WriteMask | InstMask | DataMask;
  for (size_t i = 0; i < chunkCount_; ++i)
    attribs_[i] = nirvana;
}


Memory::~Memory()
{
  if (data_)
    {
      munmap(data_, size_);
      data_ = nullptr;
      delete [] attribs_;
      attribs_ = nullptr;

      if (chunkMasks_)
	{
	  for (size_t i = 0; i < chunkCount_; ++i)
	    delete [] chunkMasks_[i];
	}
      delete [] chunkMasks_;
      chunkMasks_ = nullptr;
    }
}


bool
Memory::loadHexFile(const std::string& fileName)
{
  std::ifstream input(fileName);

  if (not input.good())
    {
      std::cerr << "Failed to open hex-file '" << fileName << "' for input\n";
      return false;
    }

  size_t address = 0, errors = 0;

  std::string line;

  for (unsigned lineNum = 0; std::getline(input, line); ++lineNum)
    {
      if (line.empty())
	continue;

      if (line[0] == '@')
	{
	  if (line.size() == 1)
	    {
	      std::cerr << "File " << fileName << ", Line " << lineNum << ": "
			<< "Invalid hexadecimal address: " << line << '\n';
	      errors++;
	      continue;
	    }
	  char* end = nullptr;
	  address = std::strtoull(line.c_str() + 1, &end, 16);
	  if (end and *end and *end != ' ')
	    {
	      std::cerr << "File " << fileName << ", Line " << lineNum << ": "
			<< "Invalid hexadecimal address: " << line << '\n';
	      errors++;
	    }
	  continue;
	}

      std::istringstream iss(line);
      uint32_t value;
      while (iss)
	{
	  iss >> std::hex >> value;
	  if (iss.fail())
	    {
	      std::cerr << "File " << fileName << ", Line " << lineNum << ": "
			<< "Invalid data: " << line << '\n';
	      errors++;
	      break;
	    }
	  if (value > 0xff)
	    {
	      std::cerr << "File " << fileName << ", Line " << lineNum << ": "
			<< "Invalid value: " << std::hex << value << '\n';
	      errors++;
	    }
	  if (address < size_)
	    {
	      if (not errors)
		data_[address++] = value;
	    }
	  else
	    {
	      std::cerr << "File " << fileName << ", Line " << lineNum << ": "
			<< "Address out of bounds: " << std::hex << address
			<< '\n';
	      errors++;
	      break;
	    }
	  if (iss.eof())
	    break;
	}

      if (iss.bad())
	{
	  std::cerr << "File " << fileName << ", Line " << lineNum << ": "
		    << "Failed to parse data line: " << line << '\n';
	  errors++;
	}
    }

  return errors == 0;
}


bool
Memory::loadElfFile(const std::string& fileName, size_t& entryPoint,
		    size_t& exitPoint,
		    std::unordered_map<std::string, size_t>& symbols)
{
  entryPoint = 0;

  ELFIO::elfio reader;

  if (not reader.load(fileName))
    {
      std::cerr << "Failed to load ELF file " << fileName << '\n';
      return false;
    }

  if (reader.get_class() != ELFCLASS32)
    {
      std::cerr << "Ony 32-bit ELF is currently supported\n";
      return false;
    }

  if (reader.get_encoding() != ELFDATA2LSB)
    {
      std::cerr << "Only little-endian ELF is currently supported\n";
      return false;
    }

  if (reader.get_machine() != EM_RISCV)
    {
      std::cerr << "Warning: non-riscv ELF file\n";
    }

  // Copy loadable ELF segments into memory.
  size_t maxEnd = 0;  // Largest end address of a segment.
  unsigned loadedSegs = 0, errors = 0;
  for (int segIx = 0; segIx < reader.segments.size(); ++segIx)
    {
      const ELFIO::segment* seg = reader.segments[segIx];
      ELFIO::Elf64_Addr vaddr = seg->get_virtual_address();
      ELFIO::Elf_Xword segSize = seg->get_file_size(); // Size in file.
      const char* segData = seg->get_data();
      if (seg->get_type() == PT_LOAD)
	{
	  if (vaddr + segSize > size_)
	    {
	      std::cerr << "End of ELF segment " << segIx << " ("
			<< (vaddr+segSize)
			<< ") is beyond end of simulated meomry ("
			<< size_ << ")\n";
	      errors++;
	    }
	  else
	    {
	      for (size_t i = 0; i < segSize; ++i)
		if (not writeByteNoAccessCheck(vaddr + i, segData[i]))
		  {
		    std::cerr << "Failed to copy ELF byte at address 0x"
			      << std::hex << (vaddr + i) << '\n';
		    errors++;
		    break;
		  }
	      loadedSegs++;
	      maxEnd = std::max(maxEnd, size_t(vaddr) + size_t(segSize));
	    }
	}
    }

  clearLastWriteInfo();

  // Collect symbols.
  auto secCount = reader.sections.size();
  for (int secIx = 0; secIx < secCount; ++secIx)
    {
      auto sec = reader.sections[secIx];
      if (sec->get_type() != SHT_SYMTAB)
	continue;

      const ELFIO::symbol_section_accessor symAccesor(reader, sec);
      ELFIO::Elf64_Addr address = 0;
      ELFIO::Elf_Xword size = 0;
      unsigned char bind, type, other;
      ELFIO::Elf_Half index = 0;

      // Finding symbol by name does not work. Walk all the symbols.
      ELFIO::Elf_Xword symCount = symAccesor.get_symbols_num();
      for (ELFIO::Elf_Xword symIx = 0; symIx < symCount; ++symIx)
	{
	  std::string name;
	  if (symAccesor.get_symbol(symIx, name, address, size, bind, type,
				    index, other))
	    symbols[name] = address;
	}
    }

  if (loadedSegs == 0)
    {
      std::cerr << "No loadable segment in ELF file\n";
      errors++;
    }

  // Get the program entry point.
  if (not errors)
    {
      entryPoint = reader.get_entry();
      exitPoint = maxEnd;
      if (symbols.count("_finish"))
	exitPoint = symbols.at("_finish");
    }

  return errors == 0;
}


bool
Memory::getElfFileAddressBounds(const std::string& fileName, size_t& minAddr,
				size_t& maxAddr)

{
  ELFIO::elfio reader;

  if (not reader.load(fileName))
    {
      std::cerr << "Failed to load ELF file " << fileName << '\n';
      return false;
    }

  // Get min max bounds of the segments.
  size_t minBound = ~ size_t(0);
  size_t maxBound = 0;
  unsigned validSegs = 0;
  for (int segIx = 0; segIx < reader.segments.size(); ++segIx)
    {
      const ELFIO::segment* seg = reader.segments[segIx];
      if (seg->get_type() != PT_LOAD)
	continue;

      ELFIO::Elf64_Addr vaddr = seg->get_virtual_address();
      ELFIO::Elf_Xword size = seg->get_file_size(); // Size in file.

      minBound = std::min(minBound, size_t(vaddr));
      maxBound = std::max(maxBound, size_t(vaddr + size));
      validSegs++;
    }

  if (validSegs == 0)
    {
      std::cerr << "No loadable segment in ELF file\n";
      return false;
    }

  minAddr = minBound;
  maxAddr = maxBound;
  return true;
}


void
Memory::copy(const Memory& other)
{
  size_t n = std::min(size_, other.size_);
  memcpy(data_, other.data_, n);
}


static
bool
checkCcmConfig(const std::string& tag, size_t region, size_t offset, size_t size,
	       size_t regionCount, unsigned& sizeCode)
{
  sizeCode = 0;
  if (size == 32*1024)
    sizeCode = 0;
  else if (size == 64*1024)
    sizeCode = 1;
  else if (size == 128*1024)
    sizeCode = 2;
  else if (size == 256*1024)
    sizeCode = 3;
  else
    {
      std::cerr << "Invalid " << tag << " size (" << size << "). Expecting\n"
		<< "  32768 (32k), 65536 (64k), 131072 (128k) or "
		<< "262144 (256k)\n";
      return false;
    }

  if (region >= regionCount)
    {
      std::cerr << "Invalid " << tag << " region (" << region << "). Expecting "
		<< "number betwen 0 and " << (regionCount - 1) << "15\n";
      return false;
    }

  if ((offset & 0x3ffff) != 0)  // Must be a multiple of 256k
    {
      std::cerr << "Invalid " << tag << " offset (" << offset;
      return false;
    }

  return true;
}
    

bool
Memory::defineIccm(size_t region, size_t offset, size_t size)
{
  unsigned sizeCode = 0;
  if (not checkCcmConfig("ICCM", region, offset, size, regionCount_, sizeCode))
    return false;

  size_t addr = region * regionSize_ + offset;
  size_t ix = getAttribIx(addr);
  if (attribs_[ix] & MappedMask)
    {
      std::cerr << "Region 0x" << std::hex << region << " offset 0x"
		<< std::hex << offset << " already mapped\n";
    }

  attribs_[ix] = sizeCode;
  attribs_[ix] |= MappedMask | InstMask;
  return true;
}


bool
Memory::defineDccm(size_t region, size_t offset, size_t size)
{
  unsigned sizeCode = 0;
  if (not checkCcmConfig("DCCM", region, offset, size, regionCount_, sizeCode))
    return false;

  size_t addr = region * regionSize_ + offset;
  size_t ix = getAttribIx(addr);
  if (attribs_[ix] & MappedMask)
    {
      std::cerr << "Region 0x" << std::hex << region << " offset 0x"
		<< std::hex << offset << " already mapped\n";
    }
	
  attribs_[ix] = sizeCode;
  attribs_[ix] |= MappedMask | WriteMask | DataMask;
  return true;
}


bool
Memory::defineMemoryMappedRegisterRegion(size_t region, size_t size,
					 size_t picBaseOffset)
{
  unsigned sizeCode = 0;
  if (size == 32*1024)
    sizeCode = 0;
  else if (size == 64*1024)
    sizeCode = 1;
  else if (size == 128*1024)
    sizeCode = 2;
  else if (size == 256*1024)
    sizeCode = 3;
  else
    {
      std::cerr << "Invalid PIC memory size (" << size << "). Expecting\n"
		<< " 32768 (32k), 65536 (64k), 131072 (128k) or "
		<< "262144 (256k)\n";
      return false;
    }

  if (region >= regionCount_)
    {
      std::cerr << "Invalid PIC memory region (" << region << "). Expecting "
		<< "number betwen 0 and " << (regionCount_ - 1) << "\n";
      return false;
    }

  if ((picBaseOffset & 0x3ffff) != 0)  // Must be a multiple of 256k
    {
      std::cerr << "Invalid PIC memory offset (" << picBaseOffset << '\n';
      return false;
    }

  size_t addr = region * regionSize_ + picBaseOffset;
  size_t ix = getAttribIx(addr);
  if (attribs_[ix] & MappedMask)
    {
      std::cerr << "Region 0x" << std::hex << region << " offset 0x"
		<< std::hex << picBaseOffset << " already mapped\n";
    }

  attribs_[ix] = sizeCode;
  attribs_[ix] |= MappedMask | WriteMask | DataMask | RegisterMask;
  return true;
}


bool
Memory::defineMemoryMappedRegisterWriteMask(size_t region,
					    size_t picBaseOffset,
					    size_t registerBlockOffset,
					    size_t registerIx,
					    uint32_t mask)
{
  size_t chunkStart = region * regionSize_ + picBaseOffset;
  size_t ix = getAttribIx(chunkStart);
  if (not (attribs_[ix] & MappedMask))
    {
      std::cerr << "Region 0x" << std::hex << region << " offset 0x"
		<< std::hex << picBaseOffset << " is not defined\n";
      return false;
    }

  if (not (attribs_[ix] & RegisterMask))
    {
      std::cerr << "Region 0x" << std::hex << region << " offset 0x"
		<< std::hex << picBaseOffset
		<< " is not for memory mapped registers\n";
      return false;
    }

  if (registerBlockOffset & 3)
    {
      std::cerr << "Memory mapped register offset (0x" << std::hex
		<< registerBlockOffset << " is not a multiple of 4\n";
      return false;
    }

  size_t expectedStart = getChunkStartAddr(chunkStart);
  if (expectedStart != chunkStart)
    {
      std::cerr << "Region 0x" << std::hex << region << " offset 0x"
		<< std::hex << picBaseOffset << " is invalid\n";
      return false;
    }

  unsigned attrib = getAttrib(chunkStart);
  size_t chunkEnd = chunkStart + attribSize(attrib);
  size_t registerEndAddr = chunkStart + registerBlockOffset + registerIx*4 + 3;
  if (registerEndAddr >= chunkEnd)
    {
      std::cerr << "PIC register out of bounds:\n"
		<< "  region:          0x" << std::hex << region << '\n'
		<< "  pic-base-offset: 0x" << std::hex << picBaseOffset << '\n'
		<< "  register-offset: 0x" << std::hex << registerBlockOffset << '\n'
		<< "  register-index:  0x" << std::hex << registerIx << '\n';
      return false;
    }

  if (not chunkMasks_)
    {
      typedef uint32_t* WordPtr;
      chunkMasks_ = new WordPtr[chunkCount_];
      for (size_t i = 0; i < chunkCount_; ++i)
	chunkMasks_[i] = nullptr;
    }

  uint32_t* masks = chunkMasks_[ix];
  if (not masks)
    {
      size_t wordCount = (chunkEnd - chunkStart) / 4;
      masks = chunkMasks_[ix] = new uint32_t[wordCount];
      for (size_t i = 0; i < wordCount; ++i)
	masks[i] = 0;
    }
  size_t blockIx = registerBlockOffset / 4;
  masks[blockIx + registerIx] = mask;

  return true;
}
