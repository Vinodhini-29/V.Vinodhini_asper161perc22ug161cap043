//
// Created by soulk on 20. 1. 20.
//


#include "media_router/bitstream/avc_video_packet_fragmentizer.h"
#include "base/info/application.h"
#include "rtspc_stream.h"

#define OV_LOG_TAG "RtspcStream"

namespace pvd
{
	std::shared_ptr<RtspcStream> RtspcStream::Create(const std::shared_ptr<pvd::Application> &application, 
		const uint32_t stream_id, const ov::String &stream_name,
		const std::vector<ov::String> &url_list)
	{
		info::Stream stream_info(*std::static_pointer_cast<info::Application>(application), StreamSourceType::RtspPull);

		stream_info.SetId(stream_id);
		stream_info.SetName(stream_name);

		auto stream = std::make_shared<RtspcStream>(application, stream_info, url_list);
		if (!stream->Start())
		{
			// Explicit deletion
			stream.reset();
			return nullptr;
		}

		return stream;
	}

	RtspcStream::RtspcStream(const std::shared_ptr<pvd::Application> &application, const info::Stream &stream_info, const std::vector<ov::String> &url_list)
	: pvd::Stream(application, stream_info)
	{
		av_register_all(); 
		avcodec_register_all(); 
		avformat_network_init(); 

		_stop_thread_flag = false;
		_state = State::IDLE;
		_format_context = 0;

		for(auto &url : url_list)
		{
			auto parsed_url = ov::Url::Parse(url.CStr());
			if(parsed_url)
			{
				_url_list.push_back(parsed_url);
			}
		}

		if(!_url_list.empty())
		{
			_curr_url = _url_list[0];
		}
	}

	RtspcStream::~RtspcStream()
	{
		Stop();
	}

	bool RtspcStream::Start()
	{
		if (_stop_thread_flag)
		{
			return false;
		}

		_stop_watch.Start();

		auto begin = std::chrono::steady_clock::now();
		if (!ConnectTo())
		{
			return false;
		}

		auto end = std::chrono::steady_clock::now();
		std::chrono::duration<double, std::milli> elapsed = end - begin;
		auto origin_request_time_msec = elapsed.count();

		begin = std::chrono::steady_clock::now();
		if (!RequestDescribe())
		{
			return false;
		}

		end = std::chrono::steady_clock::now();
		elapsed = end - begin;
		auto origin_response_time_msec = elapsed.count();

		_stream_metrics = StreamMetrics(*std::static_pointer_cast<info::Stream>(GetSharedPtr()));
		if(_stream_metrics != nullptr)
		{
			_stream_metrics->SetOriginRequestTimeMSec(origin_request_time_msec);
			_stream_metrics->SetOriginResponseTimeMSec(origin_response_time_msec);
		}

		return pvd::Stream::Start();
	}

	bool RtspcStream::Play()
	{
		if (!RequestPlay())
		{
			return false;
		}

		_stop_thread_flag = false;
		_worker_thread = std::thread(&RtspcStream::WorkerThread, this);

		return pvd::Stream::Play();
	}

	bool RtspcStream::Stop()
	{
		// Already stopping
		if(_state != State::PLAYING)
		{
			return true;
		}
		
		if(!RequestStop())
		{
			// Force terminate 
			_state = State::ERROR;
			_stop_thread_flag = true;
		}

		if(_worker_thread.joinable())
		{
			_worker_thread.join();
		}
	
		return pvd::Stream::Stop();
	}

	bool RtspcStream::ConnectTo()
	{
		if(_state != State::IDLE && _state != State::ERROR)
		{
			return false;
		}

		logti("Requested url[%d] : %s", strlen(_curr_url->Source().CStr()), _curr_url->Source().CStr() );

		_format_context = avformat_alloc_context(); 
		if(_format_context == nullptr)
		{
			_state = State::ERROR;
			logte("Canot create context");
			
			return false;
		}

		// Interrupt Callback for session termination.
		_format_context->interrupt_callback.callback = InterruptCallback;
		_format_context->interrupt_callback.opaque = this;

		AVDictionary *options = NULL;
		::av_dict_set(&options, "rtsp_transport", "tcp", 0);
		::av_dict_set_int(&options, "reorder_queue_size ", 100, 0);
		
		_format_context->flags |= AVFMT_FLAG_GENPTS;

		int err = 0;
		_stop_watch.Update();
		if ( (err = ::avformat_open_input(&_format_context, _curr_url->Source().CStr(), NULL, &options))  < 0) 
		{
			_state = State::ERROR;
			
			char errbuf[256];
			av_strerror(err, errbuf, sizeof(errbuf));

			if(_stop_watch.IsElapsed(RTSP_PULL_TIMEOUT_MSEC))
			{
				logte("Failed to connect to RTSP server.(%s/%s) : Timed out", GetApplicationInfo().GetName().CStr(), GetName().CStr(), errbuf);
			}
			else
			{
				logte("Failed to connect to RTSP server.(%s/%s) : %s", GetApplicationInfo().GetName().CStr(), GetName().CStr(), errbuf);
			}

			return false;
		}

		_state = State::CONNECTED;

		return true;
	}

	bool RtspcStream::RequestDescribe()
	{
		if(_state != State::CONNECTED)
		{
			return false;
		}

		if (::avformat_find_stream_info(_format_context, NULL) < 0) 
		{
			_state = State::ERROR;        	
			logte("Could not find stream information");

			return false;
		}

		for (uint32_t track_id = 0; track_id < _format_context->nb_streams; track_id++)
		{
			AVStream *stream = _format_context->streams[track_id];

			logtd("[%d] media_type[%d] codec_id[%d], extradata_size[%d] tb[%d/%d]", track_id, stream->codecpar->codec_type, stream->codecpar->codec_id, stream->codecpar->extradata_size, stream->time_base.num, stream->time_base.den);

			auto new_track = std::make_shared<MediaTrack>();

			common::MediaType media_type = 	(stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)?common::MediaType::Video:
			(stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)?common::MediaType::Audio:
			common::MediaType::Unknown;

			common::MediaCodecId media_codec = (stream->codecpar->codec_id == AV_CODEC_ID_H264)?common::MediaCodecId::H264:
												(stream->codecpar->codec_id == AV_CODEC_ID_VP8)?common::MediaCodecId::Vp8:
												(stream->codecpar->codec_id == AV_CODEC_ID_VP9)?common::MediaCodecId::Vp9:
												(stream->codecpar->codec_id == AV_CODEC_ID_FLV1)?common::MediaCodecId::Flv:
												(stream->codecpar->codec_id == AV_CODEC_ID_AAC)?common::MediaCodecId::Aac:
												(stream->codecpar->codec_id == AV_CODEC_ID_MP3)?common::MediaCodecId::Mp3:
												(stream->codecpar->codec_id == AV_CODEC_ID_OPUS)?common::MediaCodecId::Opus:
												common::MediaCodecId::None;


			if (media_type == common::MediaType::Unknown || media_codec == common::MediaCodecId::None)
			{
				logtp("Unknown media type or codec_id. media_type(%d), media_codec(%d)", media_type, media_codec);

				continue;
			}

			new_track->SetId(track_id);
			new_track->SetCodecId(media_codec);
			new_track->SetMediaType(media_type);
			new_track->SetTimeBase(stream->time_base.num, stream->time_base.den);
			new_track->SetBitrate(stream->codecpar->bit_rate);
			new_track->SetStartFrameTime(0);
			new_track->SetLastFrameTime(0);

			// Video Specific parameters
			if (media_type == common::MediaType::Video)
			{
				new_track->SetFrameRate(av_q2d(stream->r_frame_rate));
				new_track->SetWidth(stream->codecpar->width);
				new_track->SetHeight(stream->codecpar->height);
			}
			// Audio Specific parameters
			else if(media_type == common::MediaType::Audio)
			{
				new_track->SetSampleRate(stream->codecpar->sample_rate);
				new_track->GetSample().SetFormat(static_cast<common::AudioSample::Format>(common::AudioSample::Format::S16P));
				new_track->GetChannel().SetLayout(static_cast<common::AudioChannel::Layout>(common::AudioChannel::Layout::LayoutStereo));

#if 0
				logtd("profileid: %d, channels: %d, sample_rate:: %d", stream->codecpar->profile, stream->codecpar->channels, stream->codecpar->sample_rate);
				switch(stream->codecpar->profile)
				{
					case FF_PROFILE_AAC_MAIN:  logte("profile : %s", "MAIN"); break;
					case FF_PROFILE_AAC_LOW: logte("profile : %s", "LC"); break;
					case FF_PROFILE_AAC_SSR: logte("profile : %s", "SSR"); break;
					case FF_PROFILE_AAC_HE: logte("profile : %s", "HE-AAC"); break;
					case FF_PROFILE_AAC_HE_V2: logte("profile : %s", "HE-AACv2"); break;
					default:
						logte("profile : %s", "Unknown"); break;
				}
#endif
				// Do not crate an audio track.
				continue;
			}

			AddTrack(new_track);
		}

		_state = State::DESCRIBED;

		return true;
	}

	bool RtspcStream::RequestPlay()
	{
		if(_state != State::DESCRIBED)
		{
			return false;
		}

		_state = State::PLAYING;

		return true;
	}

	bool RtspcStream::RequestStop()
	{
		if (_state != State::PLAYING)
		{
			return false;
		}

		_state = State::STOPPING;
		_stop_thread_flag = true;

		return true;
	}

	bool RtspcStream::IsStopThread()
	{
		return _stop_thread_flag;
	}

	int RtspcStream::InterruptCallback(void *ctx)
	{
		if(ctx != nullptr)
		{
			auto obj = (RtspcStream*)ctx;

			if(obj->IsStopThread() == true)				
			{
				return true;
			}

			if(obj->_stop_watch.IsElapsed(RTSP_PULL_TIMEOUT_MSEC))
			{
				// timed out
				return true;
			}
		}

		return false;
	}

	void RtspcStream::WorkerThread()
	{
		AVPacket packet;
		av_init_packet(&packet);
		packet.size = 0;
		packet.data = nullptr;

		bool is_eof = false;
		bool is_received_first_packet = false;

		// Sometimes the values of PTS/DTS are negative or incorrect(invalid pts) 
		// Decided to calculate PTS/DTS as the cumulative value of the packet duration.
		// 
		//  It works normally in my environment.
		int64_t *cumulative_pts = new int64_t[_format_context->nb_streams];
		int64_t *cumulative_dts = new int64_t[_format_context->nb_streams];
		for(uint32_t i=0 ; i<_format_context->nb_streams ; ++i)
		{
			cumulative_pts[i] = 0;
			cumulative_dts[i] = 0;
		}

		while (true)
		{
			_stop_watch.Update();
			int32_t ret = ::av_read_frame(_format_context, &packet);
			if ( ret < 0 )
			{
				if(_stop_watch.IsElapsed(RTSP_PULL_TIMEOUT_MSEC))
				{
					logte("%s/%s(%u) : RTSP server has timed out. The thread has been terminated.", 
							GetApplicationInfo().GetName().CStr(), GetName().CStr(), GetId());
					_state = State::STOPPED;
					break;
				}

				if(IsStopThread())
				{
					logti("%s/%s(%u) RtspcStream thread has finished by signal.", 
							GetApplicationInfo().GetName().CStr(), GetName().CStr(), GetId());
					_state = State::STOPPED;
					break;
				}

				if ( (ret == AVERROR_EOF || ::avio_feof(_format_context->pb)) && !is_eof)
				{
					// If EOF is not receiving packets anymore, end thread.
					logti("%s/%s(%u) RtspcStream thread has finished.", GetApplicationInfo().GetName().CStr(), GetName().CStr(), GetId());
					_state = State::STOPPED;
					is_eof = true;
					break;
				}

				if (_format_context->pb && _format_context->pb->error)
				{
					// If the connection is broken, terminate the thread.
					logte("%s/%s(%u) RtspcStream's connection has broken. The thread has been terminated.", 
						GetApplicationInfo().GetName().CStr(), GetName().CStr(), GetId());
					_state = State::ERROR;
					break;
				}
			}
			else
			{
				is_eof = false;
			}

			// If the first packet is received as NOPTS_VALUE, reset the PTS value to zero.
			if (!is_received_first_packet)
			{
				if (packet.pts == AV_NOPTS_VALUE)
				{
					packet.pts = 0;
				}
				if (packet.dts == AV_NOPTS_VALUE)
				{	
					packet.dts = 0;
				}

				is_received_first_packet = true;
			}
			

			if(_stream_metrics != nullptr)
			{
				_stream_metrics->IncreaseBytesIn(packet.size);
			}

			auto track = GetTrack(packet.stream_index);
			if(track == nullptr)
			{
				::av_packet_unref(&packet);
				continue;
			}


			// Accumulate PTS/DTS
			// TODO : Require Verification
			if(packet.duration != 0)
			{
				cumulative_pts[packet.stream_index] += packet.duration;
				cumulative_dts[packet.stream_index] += packet.duration;
			}
			else
			{
				cumulative_pts[packet.stream_index] = packet.pts;
				cumulative_dts[packet.stream_index] = packet.dts;
			}


			AVStream *stream = _format_context->streams[packet.stream_index];
			auto media_type = track->GetMediaType();
			auto codec_id = track->GetCodecId();
			auto flag = (packet.flags & AV_PKT_FLAG_KEY) ? MediaPacketFlag::Key : MediaPacketFlag::NoFlag;
			
			// Make MediaPacket from AVPacket
			auto media_packet = std::make_shared<MediaPacket>(track->GetMediaType(), track->GetId(), packet.data, packet.size, cumulative_pts[packet.stream_index], cumulative_dts[packet.stream_index], packet.duration, flag);
			// auto media_packet = std::make_shared<MediaPacket>(track->GetMediaType(), track->GetId(), packet.data, packet.size, pkt.pts, pkt.dts, packet.duration, flag);


			// SPS/PPS Insject from Extra Data
			if(media_type == common::MediaType::Video && codec_id == common::MediaCodecId::H264)
			{
				if(stream->codecpar->extradata != nullptr && stream->codecpar->extradata_size > 0)
				{
					if(flag == MediaPacketFlag::Key)
					{
						// Inject the SPS/PPS for each keframes.
						media_packet->GetData()->Insert(stream->codecpar->extradata, 0, stream->codecpar->extradata_size);
					}
				}
			}
			else if(media_type == common::MediaType::Audio && codec_id == common::MediaCodecId::Aac)
			{
				if(stream->codecpar->extradata != nullptr && stream->codecpar->extradata_size > 0)
				{
					if(flag == MediaPacketFlag::Key)
					{
						// TODO: Inject to ADTS header for AAC bitstream							
					}
				}
			}			

			// logtp("track_id(%d), flags(%d), pts(%10lld), dts(%10lld), size(%5d), duration(%4d)"
			// 	, media_packet->GetTrackId(), media_packet->GetFlag(), media_packet->GetPts(), media_packet->GetDts(), media_packet->GetData()->GetLength(), media_packet->GetDuration());

			_application->SendFrame(GetSharedPtrAs<info::Stream>(), media_packet);

			::av_packet_unref(&packet);
		}

		if (_format_context)
		{
			avformat_close_input(&_format_context);
			_format_context = nullptr;
		}

		delete cumulative_pts;
		delete cumulative_dts;


	}

	// Generates ADTS Header
	// Reference: https://wiki.multimedia.cx/index.php?title=MPEG-4_Audio#Sampling_Frequencies
	bool RtspcStream::GenerateADTSHeader(int32_t profile, int32_t samplerate, int32_t channels)
	{
		// char aac_fixed_header[7];

		return true;
	}
}




#if 0
// enum AacObjectType
// {
//     AacObjectTypeReserved = 0,
//     // Table 1.1 - Audio Object Type definition
//     // @see @see aac-mp4a-format-ISO_IEC_14496-3+2001.pdf, page 23
//     AacObjectTypeAacMain = 1,
//     AacObjectTypeAacLC = 2,
//     AacObjectTypeAacSSR = 3,
//     // AAC HE = LC+SBR
//     AacObjectTypeAacHE = 5,
//     // AAC HEv2 = LC+SBR+PS
//     AacObjectTypeAacHEV2 = 29,
// };
// static const AVProfile aac_profiles[] = {
// 		    { FF_PROFILE_AAC_LOW,   "LC"       },
// 		    { FF_PROFILE_AAC_HE,    "HE-AAC"   },
// 		    { FF_PROFILE_AAC_HE_V2, "HE-AACv2" },
// 		    { FF_PROFILE_AAC_LD,    "LD"       },
// 		    { FF_PROFILE_AAC_ELD,   "ELD"      },
// 		    { FF_PROFILE_UNKNOWN },
// 		};

// #define FF_PROFILE_UNKNOWN -99
// #define FF_PROFILE_RESERVED -100
// #define FF_PROFILE_AAC_MAIN 0
// #define FF_PROFILE_AAC_LOW  1
// #define FF_PROFILE_AAC_SSR  2
// #define FF_PROFILE_AAC_LTP  3
// #define FF_PROFILE_AAC_HE   4
// #define FF_PROFILE_AAC_HE_V2 28
// #define FF_PROFILE_AAC_LD   22
// #define FF_PROFILE_AAC_ELD  38
// #define FF_PROFILE_MPEG2_AAC_LOW 128
// #define FF_PROFILE_MPEG2_AAC_HE  131

// const AVProfile ff_aac_profiles[] = {
//     { FF_PROFILE_AAC_LOW,   "LC"       },
//     { FF_PROFILE_AAC_HE,    "HE-AAC"   },
//     { FF_PROFILE_AAC_HE_V2, "HE-AACv2" },
//     { FF_PROFILE_AAC_LD,    "LD"       },
//     { FF_PROFILE_AAC_ELD,   "ELD"      },
//     { FF_PROFILE_AAC_MAIN,  "Main" },
//     { FF_PROFILE_AAC_SSR,   "SSR"  },
//     { FF_PROFILE_AAC_LTP,   "LTP"  },
//     { FF_PROFILE_UNKNOWN },
// };			


#endif

// Make ADTS Header
#if 0
char aac_fixed_header[7];

int8_t 						aac_profile = 1;
int8_t 						aac_sample_rate =4;
int8_t 						aac_channels = 2;
if(true)
{
	char *pp = aac_fixed_header;
	int16_t aac_frame_length = media_packet->GetData()->GetLength() + 7;

	// Syncword 12 bslbf
	*pp++ = 0xff;
	// 4bits left.
	// adts_fixed_header(), 1.A.2.2.1 Fixed Header of ADTS
	// ID 1 bslbf
	// Layer 2 uimsbf
	// protection_absent 1 bslbf
	*pp++ = 0xf1;

	// profile 2 uimsbf
	// sampling_frequency_index 4 uimsbf
	// private_bit 1 bslbf
	// channel_configuration 3 uimsbf
	// original/copy 1 bslbf
	// home 1 bslbf
	*pp++ = ((aac_profile << 6) & 0xc0) | ((aac_sample_rate << 2) & 0x3c) | ((aac_channels >> 2) & 0x01);
	// 4bits left.
	// adts_variable_header(), 1.A.2.2.2 Variable Header of ADTS
	// copyright_identification_bit 1 bslbf
	// copyright_identification_start 1 bslbf
	*pp++ = ((aac_channels << 6) & 0xc0) | ((aac_frame_length >> 11) & 0x03);

	// aac_frame_length 13 bslbf: Length of the frame including headers and error_check in bytes.
	// use the left 2bits as the 13 and 12 bit,
	// the aac_frame_length is 13bits, so we move 13-2=11.
	*pp++ = aac_frame_length >> 3;
	// adts_buffer_fullness 11 bslbf
	*pp++ = (aac_frame_length << 5) & 0xe0;

	// no_raw_data_blocks_in_frame 2 uimsbf
	*pp++ = 0xfc;
}	

media_packet->GetData()->Insert(aac_fixed_header, 0, sizeof(aac_fixed_header));
#endif	
