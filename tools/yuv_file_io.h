#ifndef TOOLS_YUV_FILE_IO_H
#define TOOLS_YUV_FILE_IO_H

#include <algorithm>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

struct vvencYUVBuffer;

typedef int16_t LPel;

static bool readYuvPlane(std::istream& fd, vvencYUVPlane& yuvPlane,
                         const int& compID) {
  const vvencChromaFormat inputChFmt = VVENC_CHROMA_420;
  const vvencChromaFormat internChFmt = VVENC_CHROMA_420;
  const int csx_file =
      ((compID == 0) || (inputChFmt == VVENC_CHROMA_444)) ? 0 : 1;
  const int csy_file =
      ((compID == 0) || (inputChFmt != VVENC_CHROMA_420)) ? 0 : 1;
  const int csx_dest =
      ((compID == 0) || (internChFmt == VVENC_CHROMA_444)) ? 0 : 1;
  const int csy_dest =
      ((compID == 0) || (internChFmt != VVENC_CHROMA_420)) ? 0 : 1;

  const int stride = yuvPlane.stride;
  const int width = yuvPlane.width;
  const int height = yuvPlane.height;
  LPel* dst = yuvPlane.ptr;

  const int fileStride = (width << csx_dest) >> csx_file;

  std::vector<uint8_t> bufVec(fileStride);
  uint8_t* buf = &(bufVec[0]);
  const unsigned mask_y_file = (1 << csy_file) - 1;
  const unsigned mask_y_dest = (1 << csy_dest) - 1;
  for (int y444 = 0; y444 < (height << csy_dest); y444++) {
    if ((y444 & mask_y_file) == 0) {
      // read a new line
      fd.read(reinterpret_cast<char*>(buf), fileStride);
      if (fd.eof() || fd.fail()) {
        return false;
      }
    }

    if ((y444 & mask_y_dest) == 0) {
      // process current destination line
      if (csx_file < csx_dest) {
        // eg file is 444, dest is 422.
        const int sx = csx_dest - csx_file;
        for (int x = 0; x < width; x++) {
          dst[x] = buf[x << sx];
        }
      } else {
        // eg file is 422, dest is 444.
        const int sx = csx_file - csx_dest;
        for (int x = 0; x < width; x++) {
          dst[x] = buf[x >> sx];
        }
      }

      dst += stride;
    }
  }
  return true;
}

// ====================================================================================================================

class YuvFileIO {
 private:
  std::string m_lastError;  ///< temporal storage for last occured error
  std::fstream m_cHandle;   ///< file handle

 public:
  int open(const std::string& fileName) {
    m_cHandle.open(fileName.c_str(), std::ios::binary | std::ios::in);

    if (m_cHandle.fail()) {
      m_lastError = "\nFailed to open input YUV file:  " + fileName;
      return -1;
    }
    return 0;
  }

  void close() { m_cHandle.close(); }
  bool isOpen() { return m_cHandle.is_open(); }
  bool isEof() { return m_cHandle.eof(); }
  bool isFail() { return m_cHandle.fail(); }
  std::string getLastError() const { return m_lastError; }

  int readYuvBuf(vvencYUVBuffer& yuvInBuf) {
    // check end-of-file
    if (isEof()) {
      m_lastError = "end of file";
      return 1;
    }

    const int numComp = 3;  // yuv420

    for (int comp = 0; comp < numComp; comp++) {
      vvencYUVPlane yuvPlane = yuvInBuf.planes[comp];

      if (!readYuvPlane(m_cHandle, yuvPlane, comp)) {
        m_lastError = "error reading YUV plane data: " + std::to_string(comp);
        return 1;
      }
    }

    return 0;
  }
};

#endif