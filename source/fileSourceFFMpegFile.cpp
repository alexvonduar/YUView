/*  This file is part of YUView - The YUV player with advanced analytics toolset
*   <https://github.com/IENT/YUView>
*   Copyright (C) 2015  Institut f�r Nachrichtentechnik, RWTH Aachen University, GERMANY
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 3 of the License, or
*   (at your option) any later version.
*
*   In addition, as a special exception, the copyright holders give
*   permission to link the code of portions of this program with the
*   OpenSSL library under certain conditions as described in each
*   individual source file, and distribute linked combinations including
*   the two.
*   
*   You must obey the GNU General Public License in all respects for all
*   of the code used other than OpenSSL. If you modify file(s) with this
*   exception, you may extend this exception to your version of the
*   file(s), but you are not obligated to do so. If you do not wish to do
*   so, delete this exception statement from your version. If you delete
*   this exception statement from all source files in the program, then
*   also delete it here.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "fileSourceFFmpegFile.h"

#include <QSettings>

#define FILESOURCEFFMPEGFILE_DEBUG_OUTPUT 0
#if FILESOURCEFFMPEGFILE_DEBUG_OUTPUT && !NDEBUG
#include <QDebug>
#define DEBUG_FFMPEG qDebug
#else
#define DEBUG_FFMPEG(fmt,...) ((void)0)
#endif

using namespace YUV_Internals;

fileSourceFFmpegFile::fileSourceFFmpegFile()
{
  fileChanged = false;
  isFileOpened = false;
  nrFrames = 0;
  posInFile = -1;
  endOfFile = false;
  duration = -1;
  timeBase.num = 0;
  timeBase.den = 0;
  frameRate = -1;
  colorConversionType = BT709_LimitedRange;

  connect(&fileWatcher, &QFileSystemWatcher::fileChanged, this, &fileSourceFFmpegFile::fileSystemWatcherFileChanged);
}

AVPacketWrapper fileSourceFFmpegFile::getNextPacket(bool getLastPackage)
{
  if (getLastPackage)
    return pkt;

  // Load the next packet
  if (!goToNextVideoPacket())
  {
    posInFile = -1;
    return AVPacketWrapper();
  }

  return pkt;
}

QByteArray fileSourceFFmpegFile::getNextNALUnit(uint64_t *pts)
{
  // Is a packet loaded?
  if (currentPacketData.isEmpty())
  {
    if (!goToNextVideoPacket())
    {
      posInFile = -1;
      return QByteArray();
    }

    currentPacketData = QByteArray::fromRawData((const char*)(pkt.get_data()), pkt.get_data_size());
    posInData = 0;
  }
  
  // FFMpeg packet use the following encoding:
  // The first 4 bytes determine the size of the NAL unit followed by the payload (ISO/IEC 14496-15)
  QByteArray sizePart = currentPacketData.mid(posInData, 4);
  unsigned int size = (unsigned char)sizePart.at(3);
  size += (unsigned char)sizePart.at(2) << 8;
  size += (unsigned char)sizePart.at(1) << 16;
  size += (unsigned char)sizePart.at(0) << 24;

  if (pts)
    *pts = pkt.get_pts();
  
  QByteArray retArray = currentPacketData.mid(posInData + 4, size);
  posInData += 4 + size;
  if (posInData >= currentPacketData.size())
    currentPacketData.clear();
  return retArray;
}

QByteArray fileSourceFFmpegFile::getExtradata()
{
  // Get the video stream
  if (!video_stream)
    return QByteArray();
  AVCodecContextWrapper codec = video_stream.getCodec();
  if (!codec)
    return QByteArray();
  return codec.get_extradata();
}

QList<QByteArray> fileSourceFFmpegFile::getParameterSets()
{
  /* The SPS/PPS are somewhere else in containers:
   * In mp4-container (mkv also) PPS/SPS are stored separate from frame data in global headers. 
   * To access them from libav* APIs you need to look for extradata field in AVCodecContext of AVStream 
   * which relate to needed video stream. Also extradata can have different format from standard H.264 
   * NALs so look in MP4-container specs for format description. */
  QByteArray extradata = getExtradata();
  QList<QByteArray> retArray;

  if (extradata.at(0) == 1)
  {
    // Internally, ffmpeg uses a custom format for the parameter sets (hvcC).
    // The hvcC parameters come first, and afterwards, the "normal" parameter sets are sent.

    // The first 22 bytes are fixed hvcC parameter set (see hvcc_write in libavformat hevc.c)
    int numOfArrays = extradata.at(22);

    int pos = 23;
    for (int i = 0; i < numOfArrays; i++)
    {
      // The first byte contains the NAL unit type (which we don't use here).
      pos++;
      //int byte = (unsigned char)(extradata.at(pos++));
      //bool array_completeness = byte & (1 << 7);
      //int nalUnitType = byte & 0x3f;

      // Two bytes numNalus
      int numNalus = (unsigned char)(extradata.at(pos++)) << 7;
      numNalus += (unsigned char)(extradata.at(pos++));

      for (int j = 0; j < numNalus; j++)
      {
        // Two bytes nalUnitLength
        int nalUnitLength = (unsigned char)(extradata.at(pos++)) << 7;
        nalUnitLength += (unsigned char)(extradata.at(pos++));

        // nalUnitLength bytes payload of the NAL unit
        // This payload includes the NAL unit header
        QByteArray rawNAL = extradata.mid(pos, nalUnitLength);
        retArray.append(rawNAL);
        pos += nalUnitLength;
      }
    }

  }

  return retArray;
}

fileSourceFFmpegFile::~fileSourceFFmpegFile()
{
  if (pkt)
    pkt.free_packet();
}

bool fileSourceFFmpegFile::openFile(const QString &filePath, fileSourceFFmpegFile *other)
{
  // Check if the file exists
  fileInfo.setFile(filePath);
  if (!fileInfo.exists() || !fileInfo.isFile())
    return false;

  if (isFileOpened)
  {
    // Close the file?
    // TODO
  }

  openFileAndFindVideoStream(filePath);
  if (!isFileOpened)
    return false;

  // Save the full file path
  fullFilePath = filePath;

  // Install a watcher for the file (if file watching is active)
  updateFileWatchSetting();
  fileChanged = false;

  // If another (already opened) bitstream is given, copy bitstream info from there; Otherwise scan the bitstream.
  if (other && other->isFileOpened)
  {
    nrFrames = other->nrFrames;
    keyFrameList = other->keyFrameList;
  }
  else
    scanBitstream();

  // Seek back to the beginning
  seekToPTS(0);

  return true;
}

// Check if we are supposed to watch the file for changes. If no, remove the file watcher. If yes, install one.
void fileSourceFFmpegFile::updateFileWatchSetting()
{
  // Install a file watcher if file watching is active in the settings.
  // The addPath/removePath functions will do nothing if called twice for the same file.
  QSettings settings;
  if (settings.value("WatchFiles",true).toBool())
    fileWatcher.addPath(fullFilePath);
  else
    fileWatcher.removePath(fullFilePath);
}

int fileSourceFFmpegFile::getClosestSeekableDTSBefore(int frameIdx, int &seekToFrameIdx) const
{
  // We are always be able to seek to the beginning of the file
  int bestSeekPTS = keyFrameList[0].pts;
  seekToFrameIdx = keyFrameList[0].frame;

  for (pictureIdx idx : keyFrameList)
  {
    if (idx.frame >= 0) 
    {
      if (idx.frame <= frameIdx)
      {
        // We could seek here
        bestSeekPTS = idx.pts;
        seekToFrameIdx = idx.frame;
      }
      else
        break;
    }
  }

  return bestSeekPTS;
}

void fileSourceFFmpegFile::scanBitstream()
{
  nrFrames = 0;
  while (goToNextVideoPacket())
  {
    DEBUG_FFMPEG("fileSourceFFmpegFile::scanBitstream: frame %d pts %d dts %d%s", nrFrames, (int)pkt.get_pts(), (int)pkt.get_dts(), pkt.get_flag_keyframe() ? " - keyframe" : "");

    if (pkt.get_flag_keyframe())
      keyFrameList.append(pictureIdx(nrFrames, pkt.get_pts()));

    nrFrames++;
  }
}

void fileSourceFFmpegFile::openFileAndFindVideoStream(QString fileName)
{
  isFileOpened = false;

  // Try to load the decoder library (.dll on Windows, .so on Linux, .dylib on Mac)
  // The libraries are only loaded on demand. This way a FFmpegLibraries instance can exist without loading the libraries.
  if (!ff.loadFFmpegLibraries())
    return;

  // Open the input file
  if (!ff.open_input(fmt_ctx, fileName))
    return;
  
  // What is the input format?
  AVInputFormatWrapper inp_format = fmt_ctx.get_input_format();

  // Get the first video stream
  for(unsigned int idx=0; idx < fmt_ctx.get_nb_streams(); idx++)
  {
    AVStreamWrapper stream = fmt_ctx.get_stream(idx);
    AVMediaType streamType =  stream.getCodecType();
    if(streamType == AVMEDIA_TYPE_VIDEO)
    {
      video_stream = stream;
      break;
    }
  }
  if(!video_stream)
    return;

  // Initialize an empty packet
  pkt.allocate_paket(ff);

  // Get the frame rate, picture size and color conversion mode
  AVRational avgFrameRate = video_stream.get_avg_frame_rate();
  if (avgFrameRate.den == 0)
    frameRate = -1;
  else
    frameRate = avgFrameRate.num / double(avgFrameRate.den);
  pixelFormat = FFmpegVersionHandler::convertAVPixelFormat(video_stream.getCodec().get_pixel_format());
  duration = fmt_ctx.get_duration();
  timeBase = video_stream.get_time_base();

  AVColorSpace colSpace = video_stream.get_colorspace();
  int w = video_stream.get_frame_width();
  int h = video_stream.get_frame_height();
  frameSize.setWidth(w);
  frameSize.setHeight(h);

  if (colSpace == AVCOL_SPC_BT2020_NCL || colSpace == AVCOL_SPC_BT2020_CL)
    colorConversionType = BT2020_LimitedRange;
  else if (colSpace == AVCOL_SPC_BT470BG || colSpace == AVCOL_SPC_SMPTE170M)
    colorConversionType = BT601_LimitedRange;
  else
    colorConversionType = BT709_LimitedRange;

  isFileOpened = true;
}

bool fileSourceFFmpegFile::goToNextVideoPacket()
{
  //Load the next video stream packet into the packet buffer
  int ret = 0;
  do
  {
    if (pkt)
      // Unref the packet
      pkt.unref_packet(ff);
  
    ret = fmt_ctx.read_frame(ff, pkt);
    DEBUG_FFMPEG("fileSourceFFmpegFile::goToNextVideoPacket: pts %d dts %d%s", (int)pkt.get_pts(), (int)pkt.get_dts(), pkt.get_flag_keyframe() ? " - keyframe" : "");
  }
  while (ret == 0 && pkt.get_stream_index() != video_stream.get_index());
  
  if (ret < 0)
  {
    endOfFile = true;
    return false;
  }
  return true;
}

bool fileSourceFFmpegFile::seekToPTS(int64_t pts)
{
  if (!isFileOpened)
    return false;

  int ret = ff.seek_frame(fmt_ctx, video_stream.get_index(), pts);
  if (ret != 0)
  {
    DEBUG_FFMPEG("FFmpegLibraries::seekToPTS Error PTS %ld. Return Code %d", pts, ret);
    return false;
  }

  // We seeked somewhere, so we are not at the end of the file anymore.
  endOfFile = false;

  DEBUG_FFMPEG("FFmpegLibraries::seekToPTS Successfully seeked to PTS %d", pts);
  return true;
}

int64_t fileSourceFFmpegFile::getMaxPTS() 
{ 
  if (!isFileOpened)
    return -1; 
  
  return duration * timeBase.den / timeBase.num / 1000;
}
