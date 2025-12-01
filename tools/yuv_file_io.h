#ifndef TOOLS_YUV_FILE_IO_H
#define TOOLS_YUV_FILE_IO_H

#include <algorithm>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "vvenc/vvenc.h"
#include "vvdec/vvdec.h"
#include "log_system/log_system.h"

struct vvencYUVBuffer;

typedef int16_t LPel;

static bool readYuvPlane(std::istream& fd, vvencYUVPlane& yuvPlane,
                         const int& compID) {
  // Assume 8-bit input YUV420 format (most common)
  const bool is16bit = false;
  const vvencChromaFormat inputChFmt = VVENC_CHROMA_420;
  const vvencChromaFormat internChFmt = VVENC_CHROMA_420;
  
  const int csx_file = ((compID == 0) || (inputChFmt == VVENC_CHROMA_444)) ? 0 : 1;
  const int csy_file = ((compID == 0) || (inputChFmt != VVENC_CHROMA_420)) ? 0 : 1;
  const int csx_dest = ((compID == 0) || (internChFmt == VVENC_CHROMA_444)) ? 0 : 1;
  const int csy_dest = ((compID == 0) || (internChFmt != VVENC_CHROMA_420)) ? 0 : 1;

  const int stride = yuvPlane.stride;
  const int width = yuvPlane.width;
  const int height = yuvPlane.height;
  LPel* dst = yuvPlane.ptr;

  // Calculate file stride accounting for bit depth
  const int fileStride = ((width << csx_dest) * (is16bit ? 2 : 1)) >> csx_file;
  // const int fileHeight = ((height << csy_dest)) >> csy_file;

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
        if (!is16bit) {
          for (int x = 0; x < width; x++) {
            dst[x] = buf[x << sx];
          }
        } else {
          for (int x = 0; x < width; x++) {
            dst[x] = LPel(buf[(x << sx) * 2 + 0]) | (LPel(buf[(x << sx) * 2 + 1]) << 8);
          }
        }
      } else {
        // eg file is 422, dest is 444.
        const int sx = csx_file - csx_dest;
        if (!is16bit) {
          for (int x = 0; x < width; x++) {
            dst[x] = buf[x >> sx];
          }
        } else {
          for (int x = 0; x < width; x++) {
            dst[x] = LPel(buf[(x >> sx) * 2 + 0]) | (LPel(buf[(x >> sx) * 2 + 1]) << 8);
          }
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

  int readYuvBuf(vvencYUVBuffer& yuvInBuf, bool& bEof) {
    // check end-of-file
    bEof = false;
    if (isEof()) {
      m_lastError = "end of file";
      bEof = true;
      return 0;
    }

    const int numComp = 3;  // yuv420

    for (int comp = 0; comp < numComp; comp++) {
      vvencYUVPlane yuvPlane = yuvInBuf.planes[comp];

      if (!readYuvPlane(m_cHandle, yuvPlane, comp)) {
        m_lastError = "error reading YUV plane data: " + std::to_string(comp);
        bEof = true;
        LOG(VERBOSE) << "[YuvFileIO::readYuvBuf] Reached end of file or read "
                     "error. bEof:"
                  << bEof;
        return 0;
      }
    }

    return 0;
  }
};

/**
 * \brief Retrieving of NAL unit start code
 */
static inline int retrieveNalStartCode( unsigned char *pB, int iZerosInStartcode )
{
  int info = 1;
  int i=0;
  for ( i = 0; i < iZerosInStartcode; i++)
  {
    if( pB[i] != 0 )
    {
      info = 0;
    }
  }

  if( pB[i] != 1 )
  {
    info = 0;
  }

  return info;
}
 
/**
* \brief Reading of one Annex B NAL unit from file stream
*/
static int readBitstreamFromFile( std::ifstream *f, vvdecAccessUnit* pcAccessUnit, bool bLoop )
{
  int info2=0;
  int info3=0;
  int pos = 0;

  int iStartCodeFound =0;
  int iRewind=0;
  uint32_t len;
  unsigned char* pBuf = pcAccessUnit->payload;
  pcAccessUnit->payloadUsedSize = 0;

  auto curfilpos = f->tellg();
  if( curfilpos < 0 )
  {
    if( bLoop )
    {
      f->clear();
      f->seekg( 0, f->beg );
      if(f->bad() || f->fail())
      {
        return -1;
      }
    }
    else
    {
      return -1;
    }
  }

  //jump over possible start code
  f->read( (char*)pBuf, 5 );
  size_t extracted = f->gcount();
  if( extracted < 4 )
  {
    if( bLoop )
    {
      f->clear();
      f->seekg( 0, f->beg );
      if(f->bad() || f->fail())
      {
        return -1;
      }

      f->read( (char*)pBuf, 5 );
      size_t extracted = f->gcount();
      if( extracted < 4 )
      {
        return -1;
      }
    }
    else
    {
      return -1;
    }
  }

  pos +=5;
  info2 = 0;
  info3 = 0;
  iStartCodeFound = 0;

  while( !iStartCodeFound )
  {
    if( f->eof() )
    {
      if( pos > 5 )
      {
        len = pos - 1;
        pcAccessUnit->payloadUsedSize=len;
        return len;
      }
      else if( bLoop )
      {
        f->clear();
        f->seekg( 0, f->beg );
      }
      else
      {
        return -1;
      }
    }

    if( pos >= pcAccessUnit->payloadSize )
    {
      int iNewSize = pcAccessUnit->payloadSize*2;
      unsigned char* newbuf = (unsigned char*) malloc( sizeof( unsigned char ) * iNewSize );
      if( newbuf == NULL )
      {
        fprintf( stderr, "ERR: readBitstreamFromFile: memory re-allocation failed!\n" );
        return -1;
      }
      std::copy_n( pcAccessUnit->payload, std::min( pcAccessUnit->payloadSize , iNewSize), newbuf);
      pcAccessUnit->payloadSize = iNewSize;
      free( pcAccessUnit->payload );

      pcAccessUnit->payload = newbuf;
      pBuf = pcAccessUnit->payload;
    }
    unsigned char* p= pBuf + pos;
    f->read( (char*)p, 1 );
    pos++;

    info3 = retrieveNalStartCode(&pBuf[pos-4], 3);
    if(info3 != 1)
    {
      info2 = retrieveNalStartCode(&pBuf[pos-3], 2);
    }
    iStartCodeFound = (info2 == 1 || info3 == 1);
  }


  // Here, we have found another start code (and read length of startcode bytes more than we should
  // have.  Hence, go back in the file
  iRewind = 0;
  if(info3 == 1)
    iRewind = -4;
  else if (info2 == 1)
    iRewind = -3;
  else
    fprintf( stderr, "ERR: readBitstreamFromFile: Error in next start code search \n");

  f->seekg ( iRewind, f->cur);
  if(f->bad() || f->fail())
  {
    fprintf( stderr, "ERR: readBitstreamFromFile: Cannot seek %d in the bit stream file", iRewind );
    return -1;
  }

  // Here the Start code, the complete NALU, and the next start code is in the pBuf.
  // The size of pBuf is pos, pos+rewind are the number of bytes excluding the next
  // start code, and (pos+rewind)-startcodeprefix_len is the size of the NALU

  len = (pos+iRewind);
  pcAccessUnit->payloadUsedSize=len;
  return len;
}

// ====================================================================================================================
// VVDec frame writing functions (for decoded frame output)

/**
 * \brief Write y4m header for a frame
 * \param[in] f Output stream pointer
 * \param[in] frame Decoded frame pointer
 * \retval int 0 on success, -1 on error
 */
static int writeY4MHeader( std::ostream *f, vvdecFrame *frame )
{
  std::stringstream cssHeader;

  if ( f == NULL || frame == NULL )
  {
    return -1;
  }

  if ( frame->sequenceNumber == 0 )
  {
    const char *cf = (frame->colorFormat == VVDEC_CF_YUV444_PLANAR) ? "444"
                   : (frame->colorFormat == VVDEC_CF_YUV422_PLANAR) ? "422" : "420";

    const char *bdepth = (frame->bitDepth == 10) ? "p10" : "";

    int frameRate=50;
    int frameScale=1;
    if( frame->picAttributes && frame->picAttributes->hrd )
    {
      if( frame->picAttributes->hrd->numUnitsInTick && frame->picAttributes->hrd->timeScale )
      {
        frameRate  = frame->picAttributes->hrd->timeScale;
        frameScale = frame->picAttributes->hrd->numUnitsInTick;

        if( frame->picAttributes->olsHrd )
        {
          frameScale *= frame->picAttributes->olsHrd->elementDurationInTc;
        }
      }
    }

    const char *interlacedMode = (frame->frameFormat == VVDEC_FF_TOP_FIELD || frame->frameFormat == VVDEC_FF_BOT_FIELD) ? " Im" : " Ip";
    cssHeader << "YUV4MPEG2 W" << frame->width << " H" << frame->height << " F" << frameRate << ":" << frameScale << interlacedMode << " C" << cf << bdepth << "\n";
  }

  cssHeader << "FRAME\n";

  f->write( cssHeader.str().c_str(),cssHeader.str().length() );

  return 0;
}

/**
 * \brief Write a component plane to file
 * \param[in] f Output stream pointer
 * \param[in] plane Plane data pointer
 * \param[in] planeField2 Second field plane (for interlaced, can be nullptr)
 * \param[in] uiBytesPerSample Bytes per sample
 * \param[in] writePYUV Whether to write in packed YUV format
 * \retval int 0 on success, non-zero on error
 */
static int _writeComponentToFile( std::ostream *f, vvdecPlane *plane, vvdecPlane *planeField2, uint32_t uiBytesPerSample, bool writePYUV )
{
  uint32_t uiWidth  = plane->width;
  uint32_t uiHeight = plane->height;

  if( plane->bytesPerSample == 2 )
  {
     unsigned short* p  = reinterpret_cast<unsigned short*>( plane->ptr );
     unsigned short* p2  = planeField2 ? reinterpret_cast<unsigned short*>( planeField2->ptr ) : nullptr;

     if( uiBytesPerSample == 1 )  // cut to 8bit output
     {
       // 8bit > 16bit conversion
       std::vector<unsigned char> tmp;
       std::vector<unsigned char> tmp2;
       tmp.resize(uiWidth);
       if( planeField2 )
       {
         tmp2.resize(uiWidth);
       }

       for( uint32_t y = 0; y < uiHeight; y++ )
       {
         for( uint32_t x = 0; x < uiWidth; x++ )
         {
           tmp[x] = (unsigned char)(p[x]>>2);
         }
         f->write( (char*)&tmp[0], sizeof(std::vector<unsigned char>::value_type)*tmp.size());
         p += plane->stride;

         if( p2 )
         {
           for( uint32_t x = 0; x < uiWidth; x++ )
           {
             tmp2[x] = (unsigned char)(p2[x]>>2);
           }
           f->write( (char*)&tmp2[0], sizeof(std::vector<unsigned char>::value_type)*tmp2.size());
           p2 += planeField2->stride;
         }
       }
     }
     else if( writePYUV)
     {
       unsigned short* p  = reinterpret_cast<unsigned short*>( plane->ptr );
       unsigned short* p2 = reinterpret_cast<unsigned short*>( planeField2 ? planeField2->ptr : nullptr );

       std::vector<unsigned char> bufVec(5);
       unsigned char *buf=&(bufVec[0]);
       int64_t iTemp = 0;

       for( uint32_t y = 0; y < uiHeight; y++ )
       {
         for (uint32_t x = 0; x < uiWidth; x+=4)
         {
           // write 4 values(8 bytes) into 5 bytes
           iTemp = (((int64_t)p[x+0])<<0) + (((int64_t)p[x+1])<<10) + (((int64_t)p[x+2])<<20) + (((int64_t)p[x+3])<<30);
           buf[0] = (iTemp >> 0  ) & 0xff;
           buf[1] = (iTemp >> 8  ) & 0xff;
           buf[2] = (iTemp >> 16 ) & 0xff;
           buf[3] = (iTemp >> 24 ) & 0xff;
           buf[4] = (iTemp >> 32 ) & 0xff;
           f->write( (char*)buf, 5 );
         }
         p += (plane->stride >> 1);

         if( p2 )
         {
           for (uint32_t x = 0; x < uiWidth; x+=4)
           {
            iTemp = (((int64_t)p[x+0])<<0) + (((int64_t)p[x+1])<<10) + (((int64_t)p[x+2])<<20) + (((int64_t)p[x+3])<<30);
            buf[0] = (iTemp >> 0  ) & 0xff;
            buf[1] = (iTemp >> 8  ) & 0xff;
            buf[2] = (iTemp >> 16 ) & 0xff;
            buf[3] = (iTemp >> 24 ) & 0xff;
            buf[4] = (iTemp >> 32 ) & 0xff;
            f->write( (char*)buf, 5 );
           }
           p2 += (planeField2->stride >> 1);
         }
       }
     }
     else
     {
       uint8_t *p = plane->ptr;
       uint8_t *p2 = planeField2 ? planeField2->ptr : nullptr;

       for( uint32_t y = 0; y < uiHeight; y++ )
       {
         f->write( (char*)p, uiWidth*uiBytesPerSample );
         p  += plane->stride;

         if( p2 )
         {
           f->write( (char*)p2, uiWidth*uiBytesPerSample );
           p2 += planeField2->stride;
         }
       }
     }
  }
  else
  {
   uint8_t *p = plane->ptr;
   uint8_t *p2 = planeField2 ? planeField2->ptr : nullptr;

   if( uiBytesPerSample == 2 )
   {
     // 8bit > 16bit conversion
     std::vector<short> tmp;
     std::vector<short> tmp2;
     tmp.resize(uiWidth);

     if( p2 )
     {
       tmp2.resize(uiWidth);
     }

     for( uint32_t y = 0; y < uiHeight; y++ )
     {
       for( uint32_t x = 0; x < uiWidth; x++ )
       {
         tmp[x] = p[x];
       }
       f->write( (char*)&tmp[0], sizeof(std::vector<short>::value_type)*tmp.size());
       p += plane->stride;

       if( p2 )
       {
         for( uint32_t x = 0; x < uiWidth; x++ )
         {
           tmp2[x] = p2[x];
         }
         f->write( (char*)&tmp2[0], sizeof(std::vector<short>::value_type)*tmp2.size());
         p2 += planeField2->stride;
       }
     }
   }
   else if( writePYUV)
   {
     // 8bit > 10bit packed conversion
     unsigned char* p  = plane->ptr ;
     unsigned char* p2 = planeField2 ? planeField2->ptr : nullptr;
     std::vector<unsigned char> bufVec(5);
     unsigned char *buf=&(bufVec[0]);
     int64_t iTemp = 0;

     for( uint32_t y = 0; y < uiHeight; y++ )
     {
       for (uint32_t x = 0; x < uiWidth; x+=4)
       {
         // write 4 values(4 bytes) into 5 bytes
         const unsigned short src0 = (unsigned short)(p[x+0]<<2);
         const unsigned short src1 = (unsigned short)(p[x+1]<<2);
         const unsigned short src2 = (unsigned short)(p[x+2]<<2);
         const unsigned short src3 = (unsigned short)(p[x+3]<<2);
         iTemp = (((int64_t)src0)<<0) + (((int64_t)src1)<<10) + (((int64_t)src2)<<20) + (((int64_t)src3)<<30);
         buf[0] = (iTemp >> 0  ) & 0xff;
         buf[1] = (iTemp >> 8  ) & 0xff;
         buf[2] = (iTemp >> 16 ) & 0xff;
         buf[3] = (iTemp >> 24 ) & 0xff;
         buf[4] = (iTemp >> 32 ) & 0xff;
         f->write( (char*)buf, 5 );
       }
       p += plane->stride;

       if( p2 )
       {
         for (uint32_t x = 0; x < uiWidth; x+=4)
         {
           const unsigned short src0 = (unsigned short)(p2[x+0]<<2);
           const unsigned short src1 = (unsigned short)(p2[x+1]<<2);
           const unsigned short src2 = (unsigned short)(p2[x+2]<<2);
           const unsigned short src3 = (unsigned short)(p2[x+3]<<2);
           iTemp = (((int64_t)src0)<<0) + (((int64_t)src1)<<10) + (((int64_t)src2)<<20) + (((int64_t)src3)<<30);
           buf[0] = (iTemp >> 0  ) & 0xff;
           buf[1] = (iTemp >> 8  ) & 0xff;
           buf[2] = (iTemp >> 16 ) & 0xff;
           buf[3] = (iTemp >> 24 ) & 0xff;
           buf[4] = (iTemp >> 32 ) & 0xff;
           f->write( (char*)buf, 5 );
         }
         p2 += planeField2->stride;
       }
     }
   }
   else
   {
     for( uint32_t y = 0; y < uiHeight; y++ )
     {
       f->write( (char*)p, uiBytesPerSample*uiWidth );
       p += plane->stride;

       if( p2 )
       {
         f->write( (char*)p2, uiBytesPerSample*uiWidth );
         p2 += planeField2->stride;
       }
     }
   }
  }
  return 0;
}

/**
 * \brief Write decoded YUV frame to file
 * \param[in] f Output stream pointer
 * \param[in] frame Decoded frame pointer
 * \param[in] y4mFormat Whether to write in y4m format
 * \param[in] pyuvOutput Whether to write in packed YUV format
 * \retval int 0 on success, non-zero on error
 */
static int writeYUVToFile( std::ostream *f, vvdecFrame *frame, bool y4mFormat, bool pyuvOutput )
{
  uint32_t c = 0;

  uint32_t uiBytesPerSample = 1;

  if( y4mFormat )
  {
    writeY4MHeader( f, frame );
  }

  for( c = 0; c < frame->numPlanes; c++ )
  {
    uiBytesPerSample = std::max( (uint32_t) frame->planes[c].bytesPerSample, uiBytesPerSample );
  }

  for( c = 0; c < frame->numPlanes; c++ )
  {
    int ret;
    if( ( ret = _writeComponentToFile( f, &frame->planes[c], nullptr, uiBytesPerSample, pyuvOutput ) ) != 0 )
    {
      return ret;
    }
  }

  return 0;
}

#endif