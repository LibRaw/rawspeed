/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post
    Copyright (C) 2014 Pedro Côrte-Real
    Copyright (C) 2017 Axel Waggershauser

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
*/

#include "parsers/TiffParser.h"
#include "common/Common.h"               // for TrimSpaces, make_unique
#include "decoders/ArwDecoder.h"         // for ArwDecoder
#include "decoders/Cr2Decoder.h"         // for Cr2Decoder
#include "decoders/DcrDecoder.h"         // for DcrDecoder
#include "decoders/DcsDecoder.h"         // for DcsDecoder
#include "decoders/DngDecoder.h"         // for DngDecoder
#include "decoders/ErfDecoder.h"         // for ErfDecoder
#include "decoders/KdcDecoder.h"         // for KdcDecoder
#include "decoders/MefDecoder.h"         // for MefDecoder
#include "decoders/MosDecoder.h"         // for MosDecoder
#include "decoders/NefDecoder.h"         // for NefDecoder
#include "decoders/OrfDecoder.h"         // for OrfDecoder
#include "decoders/PefDecoder.h"         // for PefDecoder
#include "decoders/RafDecoder.h"         // for RafDecoder
#include "decoders/Rw2Decoder.h"         // for Rw2Decoder
#include "decoders/SrwDecoder.h"         // for SrwDecoder
#include "decoders/ThreefrDecoder.h"     // for ThreefrDecoder
#include "io/ByteStream.h"               // for ByteStream
#include "io/FileMap.h"                  // for FileMap
#include "parsers/TiffParserException.h" // for TiffParserException
#include "tiff/TiffEntry.h"              // for TiffEntry
#include "tiff/TiffTag.h"                // for ::MAKE, ::MODEL, ::DNGVERSION
#include <cstdint>                       // for UINT32_MAX
#include <memory>                        // for unique_ptr
#include <stdexcept>                     // for runtime_error
#include <string>                        // for operator==, basic_string
#include <vector>                        // for vector
// IWYU pragma: no_include <ext/alloc_traits.h>

using namespace std;

namespace RawSpeed {

class RawDecoder;

TiffRootIFDOwner parseTiff(const Buffer &data) {
  ByteStream bs(data, 0);
  bs.setInNativeByteOrder(isTiffInNativeByteOrder(bs, 0, "TIFF header"));
  bs.skipBytes(2);

  ushort16 magic = bs.getShort();
  if (magic != 42 && magic != 0x4f52 && magic != 0x5352 && magic != 0x55) // ORF has 0x4f52/0x5352, RW2 0x55 - Brillant!
    throw TiffParserException("Not a TIFF file (magic 42)");

  TiffRootIFDOwner root = make_unique<TiffRootIFD>(bs, UINT32_MAX); // tell TiffIFD constructur not to parse bs as IFD
  for( uint32 nextIFD = bs.getUInt(); nextIFD; nextIFD = root->getSubIFDs().back()->getNextIFD() ) {
    root->add(make_unique<TiffIFD>(bs, nextIFD, root.get()));
  }

  return root;
}

RawDecoder* makeDecoder(TiffRootIFDOwner _root, Buffer &data) {
  TiffRootIFD* root = _root.release();
  FileMap* mInput = &data;
  if (!root)
    throw TiffParserException("TiffIFD is null.");

  if (root->hasEntryRecursive(DNGVERSION)) {  // We have a dng image entry
    try {
      return new DngDecoder(root, mInput);
    } catch (std::runtime_error& e) {
      //TODO: remove this exception type conversion
      throw TiffParserException(e.what());
    }
  }

  for (TiffIFD* ifd : root->getIFDsWithTag(MAKE)) {
    string make = ifd->getEntry(MAKE)->getString();
    TrimSpaces(make);
    string model = "";
    if (ifd->hasEntry(MODEL)) {
      model = ifd->getEntry(MODEL)->getString();
      TrimSpaces(model);
    }
    if (make == "Canon") {
      return new Cr2Decoder(root, mInput);
    }
    if (make == "FUJIFILM") {
      return new RafDecoder(root, mInput);
    }
    if (make == "NIKON CORPORATION" || make == "NIKON") {
      return new NefDecoder(root, mInput);
    }
    if (make == "OLYMPUS IMAGING CORP." || make == "OLYMPUS CORPORATION" ||
        make == "OLYMPUS OPTICAL CO.,LTD") {
      return new OrfDecoder(root, mInput);
    }
    if (make == "SONY") {
      return new ArwDecoder(root, mInput);
    }
    if (make == "PENTAX Corporation" || make == "RICOH IMAGING COMPANY, LTD." ||
        make == "PENTAX") {
      return new PefDecoder(root, mInput);
    }
    if (make == "Panasonic" || make == "LEICA") {
      return new Rw2Decoder(root, mInput);
    }
    if (make == "SAMSUNG") {
      return new SrwDecoder(root, mInput);
    }
    if (make == "Mamiya-OP Co.,Ltd.") {
      return new MefDecoder(root, mInput);
    }
    if (make == "Kodak") {
      if (model == "DCS560C")
        return new Cr2Decoder(root, mInput);
      else
        return new DcrDecoder(root, mInput);
    }
    if (make == "KODAK") {
      return new DcsDecoder(root, mInput);
    }
    if (make == "EASTMAN KODAK COMPANY") {
      return new KdcDecoder(root, mInput);
    }
    if (make == "SEIKO EPSON CORP.") {
      return new ErfDecoder(root, mInput);
    }
    if (make == "Hasselblad") {
      return new ThreefrDecoder(root, mInput);
    }
    if (make == "Leaf" || make == "Phase One A/S") {
      return new MosDecoder(root, mInput);
    }
  }

  // Last ditch effort to identify Leaf cameras that don't have a Tiff Make set
  TiffEntry* softwareIFD = root->getEntryRecursive(SOFTWARE);
  if (softwareIFD) {
    string software = softwareIFD->getString();
    TrimSpaces(software);
    if (software == "Camera Library") {
      return new MosDecoder(root, mInput);
    }
  }

  delete root;

  throw TiffParserException("No decoder found. Sorry.");
  return nullptr;
}

} // namespace RawSpeed