#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>
#include <sstream>

#include "vvdec/vvdec.h"
#include "log_system/log_system.h"
#include "tools/yuv_file_io.h"


#define MAX_CODED_PICTURE_SIZE  800000

static bool handle_frame( vvdecFrame*                   pcFrame,
                          vvdecDecoder*                 dec,
                          std::ofstream& outStream)
{
    if( pcFrame->frameFormat == VVDEC_FF_PROGRESSIVE )
    {
      vvdecFrame* outputFrame = pcFrame;

      if( outStream )
      {
        if( 0 != writeYUVToFile( &outStream, outputFrame, true, false ) )
        {
          LOG(ERROR) << "vvdecapp [error]: write of rec. yuv failed for picture seq. " <<  outputFrame->sequenceNumber;
          return false;
        }
      }
    }
    else
    {
      LOG(ERROR) << "vvdecapp [error]: unsupported FrameFormat " << pcFrame->frameFormat << " for picture seq. " <<  pcFrame->sequenceNumber;
      return false;
    }
  return true;
}

int main()
{
  std::string input_file = "result/output.266";
  std::string output_file = "result/rec.y4m";
  
  LOG(INFO) << "[TestDecoder] Input file: " << input_file;
  LOG(INFO) << "[TestDecoder] Output file: " << output_file;

  std::ofstream out_file(output_file, std::ios::binary);
  if (!out_file.is_open()) {
    LOG(ERROR) << "[TestDecoder] Failed to open output file: " << output_file;
    return -1;
  }

  vvdecParams params;
  vvdec_params_default( &params );

  params.logLevel = VVDEC_NOTICE;
  params.enable_realtime = true;

  int iRet = -1;

  // open input file
  std::ifstream cInFile( input_file.c_str(), std::fstream::binary );
  if( !cInFile )
  {
    std::cerr << "vvdecapp [error]: failed to open bitstream file " << input_file << std::endl;
    return -1;
  }

  vvdecDecoder *dec = nullptr;

  //> decoding loop
  vvdecAccessUnit* accessUnit = vvdec_accessUnit_alloc();
  vvdec_accessUnit_alloc_payload( accessUnit, MAX_CODED_PICTURE_SIZE );

  vvdecFrame* pcFrame     = NULL;
  dec = vvdec_decoder_open( &params );

  if( nullptr == dec )
  {
    LOG(ERROR) << "vvdecapp [error]: cannot init decoder";
    vvdec_accessUnit_free( accessUnit );
    return -1;
  }

  bool bFlushDecoder = false;

  accessUnit->cts = 0; accessUnit->ctsValid = true;
  accessUnit->dts = 0; accessUnit->dtsValid = true;


#if DEC_AT_FPS
  hi_res_time_point first_frame_time;
  LateFrames lateFrames;
#endif

  int iRead = 0;
  do
  {
    iRead = readBitstreamFromFile( &cInFile, accessUnit, false );
    LOG(INFO) << "vvdecapp [info]: read " << iRead << " bytes";
    //if( iRead > 0 )
    {
      vvdecNalType eNalType = vvdec_get_nal_unit_type( accessUnit );
      bool bIsSlice  = vvdec_is_nal_unit_slice( eNalType );
      // call decode
      iRet = vvdec_decode( dec, accessUnit, &pcFrame );
      if( bIsSlice )
      {
        accessUnit->cts++;
        accessUnit->dts++;
      }

      // check success
      if( iRet == VVDEC_EOF )
      {
        LOG(INFO) << "vvdecapp [info]: flush";
        bFlushDecoder = true;
      }
      else if( iRet == VVDEC_TRY_AGAIN )
      {
        LOG(WARNING) << "vvdecapp [warning]: try again";
      }
      else if( iRet == VVDEC_ERR_DEC_INPUT )
      {
        std::string cErr           = vvdec_get_last_error( dec );
        std::string cAdditionalErr = vvdec_get_last_additional_error( dec );
        LOG(WARNING) << "vvdecapp [warning]: " << cErr << " (" << vvdec_get_error_msg( iRet ) << ")";
        if( !cAdditionalErr.empty() )
        {
          LOG(INFO) << " detail: " << vvdec_get_last_additional_error( dec );
        }
      }
      else if( iRet != VVDEC_OK )
      {
        std::string cErr           = vvdec_get_last_error( dec );
        std::string cAdditionalErr = vvdec_get_last_additional_error( dec );
        LOG(ERROR) << "vvdecapp [error]: decoding failed: " << cErr << " (" <<vvdec_get_error_msg( iRet ) << ")";
        if( !cAdditionalErr.empty() )
        {
          LOG(INFO) << " detail: " << vvdec_get_last_additional_error( dec );
        }

        vvdec_accessUnit_free( accessUnit );
        return iRet;
      }


      if( pcFrame && pcFrame->ctsValid )
      {
        LOG(INFO) << "vvdecapp [info]: decoded frame valid: " << pcFrame->width << "x" << pcFrame->height;
        if( !handle_frame( pcFrame, dec, out_file))
        {
          vvdec_accessUnit_free( accessUnit );
          return -1;
        }
      }
    }
  } while( iRead > 0 && !bFlushDecoder );   // end for frames

  // flush the decoder

  // un-initialize the decoder
  iRet = vvdec_decoder_close(dec);
  if( 0 != iRet )
  {
    LOG(ERROR) << "vvdecapp [error]: cannot uninit decoder (" << iRet << ")";
    vvdec_accessUnit_free( accessUnit );
    return iRet;
  }

  // free memory of access unit
  vvdec_accessUnit_free( accessUnit );

  cInFile.close();

  return 0;
}