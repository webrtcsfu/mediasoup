#define MS_CLASS "RTC::Transport"
// #define MS_LOG_DEV

#include "RTC/Transport.hpp"
#include "DepLibUV.hpp"
#include "Logger.hpp"
#include "MediaSoupError.hpp"
#include "Utils.hpp"
#include "RTC/Consumer.hpp"
#include "RTC/Producer.hpp"
#include "RTC/RTCP/FeedbackPs.hpp"
#include "RTC/RTCP/FeedbackPsAfb.hpp"
#include "RTC/RTCP/FeedbackPsRemb.hpp"
#include "RTC/RTCP/FeedbackRtp.hpp"
#include "RTC/RTCP/FeedbackRtpNack.hpp"
#include "RTC/RtpDictionaries.hpp"

namespace RTC
{
	/* Instance methods. */

	Transport::Transport(uint32_t id, Listener* listener) : id(id), listener(listener)
	{
		MS_TRACE();

		// Create the RTCP timer.
		this->rtcpTimer = new Timer(this);
	}

	Transport::~Transport()
	{
		MS_TRACE();

		// The destructor must delete and clear everything silently.

		// Delete all Producers.
		for (auto& kv : this->mapProducers)
		{
			auto* producer = kv.second;

			delete producer;
		}
		this->mapProducers.clear();

		// Delete all Consumers.
		for (auto& kv : this->mapConsumers)
		{
			auto* consumer = kv.second;

			delete consumer;
		}
		this->mapConsumers.clear();

		// Delete the RTCP timer.
		if (this->rtcpTimer != nullptr)
			delete this->rtcpTimer;
	}

	void Transport::Close()
	{
		MS_TRACE();

		// This method is called by the Router and must notify him about all the
		// Producers and Consumers that we are gonna close.
		//
		// The caller is supposed to delete this Transport instance after calling
		// Close().

		// Close all Producers.
		for (auto& kv : this->mapProducers)
		{
			auto* producer = kv.second;

			// Notify the listener.
			this->listener->OnTransportProducerClosed(this, producer);

			delete producer;
		}
		this->mapProducers.clear();

		// Delete all Consumers.
		for (auto& kv : this->mapConsumers)
		{
			auto* consumer = kv.second;

			// Notify the listener.
			this->listener->OnTransportConsumerClosed(this, consumer);

			delete consumer;
		}
		this->mapConsumers.clear();

		// Close the RTCP timer.
		if (this->rtcpTimer != nullptr)
			this->rtcpTimer->Close();
	}

	void Router::HandleRequest(Channel::Request* request)
	{
		MS_TRACE();

		switch (request->methodId)
		{
			case Channel::Request::MethodId::TRANSPORT_SET_MAX_INCOMING_BITRATE:
			{
				static constexpr uint32_t MinBitrate{ 10000 };

				auto jsonBitrateIt = request->data.find("bitrate");

				if (jsonBitrateIt == request->data.end() || !jsonBitrateIt->is_number_unsigned())
				{
					request->Reject("missing bitrate");

					return;
				}

				uint32_t bitrate = jsonBitrateIt->get<uint32_t>();

				if (bitrate < MinBitrate)
					bitrate = MinBitrate;

				this->maxIncomingBitrate = bitrate;

				MS_DEBUG_TAG(rbe, "Transport maximum incoming bitrate set to %" PRIu32 "bps", bitrate);

				request->Accept();

				break;
			}

			case Channel::Request::MethodId::PRODUCER_CLOSE:
			{
				RTC::Producer* producer;

				try
				{
					producer = GetProducerFromRequest(request);
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				// Remove it from the map.
				this->mapProducers.erase(producer->id);

				// Remove it from the RtpListener.
				this->rtpListener.RemoveProducer(producer);

				// Notify the listener.
				this->listener->OnTransportProducerClosed(this, producer);

				// Delete it.
				delete producer;

				MS_DEBUG_DEV("Producer closed [id:%s]", producer->id.c_str());

				request->Accept();

				break;
			}

			case Channel::Request::MethodId::CONSUMER_CLOSE:
			{
				RTC::Consumer* producer;

				try
				{
					producer = GetConsumerFromRequest(request);
				}
				catch (const MediaSoupError& error)
				{
					request->Reject(error.what());

					return;
				}

				// Remove it from the map.
				this->mapConsumers.erase(consumer->id);

				// Notify the listener.
				this->listener->OnTransportConsumerClosed(this, consumer);

				// Delete it.
				delete consumer;

				MS_DEBUG_DEV("Consumer closed [id:%s]", consumer->id.c_str());

				request->Accept();

				break;
			}

			default:
			{
				MS_ERROR("unknown method '%s'", request->method);

				request->Reject("unknown method");
			}
		}
	}

	// TODO: Not here but in HandleRequest() in TRANSPORT_PRODUCE.
	void Transport::HandleProducer(RTC::Producer* producer)
	{
		MS_TRACE();

		// Pass it to the RtpListener.
		// NOTE: This may throw.
		this->rtpListener.AddProducer(producer);

		// Add to the map.
		this->mapProducers[producer->id] = producer;

		// Add us as listener.
		// producer->AddListener(this);

		// Take the transport related RTP header extension ids of the Producer
		// and add them to the Transport.

		auto& producerRtpHeaderExtensionIds = producer->GetRtpHeaderExtensionIds();

		if (producerRtpHeaderExtensionIds.absSendTime != 0u)
			this->rtpHeaderExtensionIds.absSendTime = producerRtpHeaderExtensionIds.absSendTime;

		if (producerRtpHeaderExtensionIds.mid != 0u)
			this->rtpHeaderExtensionIds.mid = producerRtpHeaderExtensionIds.mid;

		if (producerRtpHeaderExtensionIds.rid != 0u)
			this->rtpHeaderExtensionIds.rid = producerRtpHeaderExtensionIds.rid;
	}

	// TODO: Not here but in HandleRequest() in TRANSPORT_CONSUME.
	void Transport::HandleConsumer(RTC::Consumer* consumer)
	{
		MS_TRACE();

		// Add to the map.
		this->mapConsumers[consumer->id] = consumer;

		// Add us as listener.
		// consumer->AddListener(this);

		// TODO: WHAT? started here?
		//
		// If we are connected, ask a key request for this started Consumer.
		if (IsConnected())
		{
			if (consumer->kind == RTC::Media::Kind::VIDEO)
			{
				MS_DEBUG_2TAGS(
				  rtcp, rtx, "requesting key frame for new Consumer since Transport already connected");
			}

			// TODO: NO: It must call listener->OnTransportConsumerKeyFrameRequested(this, consumer)
			// without ssrc (so for all streams).
			consumer->RequestKeyFrame();
		}
	}

	void Transport::SetNewProducerIdFromRequest(Channel::Request* request, std::string& producerId) const
	{
		MS_TRACE();

		auto jsonProducerIdIt = request->internal.find("producerId");

		if (jsonProducerIdIt == request->internal.end() || !jsonProducerIdIt->is_string())
			MS_THROW_ERROR("request has no internal.producerId");

		producerId.assign(jsonProducerIdIt->get<std::string>());

		if (this->mapProducers.find(producerId) != this->mapProducers.end())
			MS_THROW_ERROR("a Producer with same producerId already exists");
	}

	RTC::Producer* Transport::GetProducerFromRequest(Channel::Request* request) const
	{
		MS_TRACE();

		auto jsonProducerIdIt = request->internal.find("producerId");

		if (jsonProducerIdIt == request->internal.end() || !jsonProducerIdIt->is_string())
			MS_THROW_ERROR("request has no internal.producerId");

		auto it = this->mapProducers.find(jsonProducerIdIt->get<std::string>());

		if (it == this->mapProducers.end())
			MS_THROW_ERROR("Producer not found");

		RTC::Producer* producer = it->second;

		return producer;
	}

	void Transport::SetNewConsumerIdFromRequest(Channel::Request* request, std::string& consumerId) const
	{
		MS_TRACE();

		auto jsonConsumerIdIt = request->internal.find("consumerId");

		if (jsonConsumerIdIt == request->internal.end() || !jsonConsumerIdIt->is_string())
			MS_THROW_ERROR("request has no internal.consumerId");

		consumerId.assign(jsonConsumerIdIt->get<std::string>());

		if (this->mapConsumers.find(consumerId) != this->mapConsumers.end())
			MS_THROW_ERROR("a Consumer with same consumerId already exists");
	}

	RTC::Consumer* Transport::GetConsumerFromRequest(Channel::Request* request) const
	{
		MS_TRACE();

		auto jsonConsumerIdIt = request->internal.find("consumerId");

		if (jsonConsumerIdIt == request->internal.end() || !jsonConsumerIdIt->is_string())
			MS_THROW_ERROR("request has no internal.consumerId");

		auto it = this->mapConsumers.find(jsonConsumerIdIt->get<std::string>());

		if (it == this->mapConsumers.end())
			MS_THROW_ERROR("Consumer not found");

		RTC::Consumer* consumer = it->second;

		return consumer;
	}

	void Transport::ReceiveRtcpPacket(RTC::RTCP::Packet* packet)
	{
		MS_TRACE();

		switch (packet->GetType())
		{
			case RTCP::Type::RR:
			{
				auto* rr = dynamic_cast<RTCP::ReceiverReportPacket*>(packet);
				auto it  = rr->Begin();

				for (; it != rr->End(); ++it)
				{
					auto& report   = (*it);
					auto* consumer = GetStartedConsumer(report->GetSsrc());

					if (consumer == nullptr)
					{
						MS_WARN_TAG(
						  rtcp,
						  "no Consumer found for received Receiver Report [ssrc:%" PRIu32 "]",
						  report->GetSsrc());

						break;
					}

					consumer->ReceiveRtcpReceiverReport(report);
				}

				break;
			}

			case RTCP::Type::PSFB:
			{
				auto* feedback = dynamic_cast<RTCP::FeedbackPsPacket*>(packet);

				switch (feedback->GetMessageType())
				{
					case RTCP::FeedbackPs::MessageType::PLI:
					case RTCP::FeedbackPs::MessageType::FIR:
					{
						auto* consumer = GetStartedConsumer(feedback->GetMediaSsrc());

						if (consumer == nullptr)
						{
							MS_WARN_TAG(
							  rtcp,
							  "no Consumer found for received %s Feedback packet "
							  "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
							  RTCP::FeedbackPsPacket::MessageType2String(feedback->GetMessageType()).c_str(),
							  feedback->GetMediaSsrc(),
							  feedback->GetMediaSsrc());

							break;
						}

						MS_DEBUG_2TAGS(
						  rtcp,
						  rtx,
						  "%s received, requesting key frame for Consumer "
						  "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
						  RTCP::FeedbackPsPacket::MessageType2String(feedback->GetMessageType()).c_str(),
						  feedback->GetMediaSsrc(),
						  feedback->GetMediaSsrc());

						consumer->ReceiveKeyFrameRequest(feedback->GetMessageType());

						break;
					}

					case RTCP::FeedbackPs::MessageType::AFB:
					{
						auto* afb = dynamic_cast<RTCP::FeedbackPsAfbPacket*>(feedback);

						// Store REMB info.
						if (afb->GetApplication() == RTCP::FeedbackPsAfbPacket::Application::REMB)
						{
							auto* remb = dynamic_cast<RTCP::FeedbackPsRembPacket*>(afb);

							this->availableOutgoingBitrate = remb->GetBitrate();

							break;
						}
						else
						{
							MS_WARN_TAG(
							  rtcp,
							  "ignoring unsupported %s Feedback PS AFB packet "
							  "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
							  RTCP::FeedbackPsPacket::MessageType2String(feedback->GetMessageType()).c_str(),
							  feedback->GetMediaSsrc(),
							  feedback->GetMediaSsrc());

							break;
						}
					}

					default:
					{
						MS_WARN_TAG(
						  rtcp,
						  "ignoring unsupported %s Feedback packet "
						  "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
						  RTCP::FeedbackPsPacket::MessageType2String(feedback->GetMessageType()).c_str(),
						  feedback->GetMediaSsrc(),
						  feedback->GetMediaSsrc());
					}
				}

				break;
			}

			case RTCP::Type::RTPFB:
			{
				auto* feedback = dynamic_cast<RTCP::FeedbackRtpPacket*>(packet);
				auto* consumer = GetStartedConsumer(feedback->GetMediaSsrc());

				if (consumer == nullptr)
				{
					MS_WARN_TAG(
					  rtcp,
					  "no Consumer found for received Feedback packet "
					  "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
					  feedback->GetMediaSsrc(),
					  feedback->GetMediaSsrc());

					break;
				}

				switch (feedback->GetMessageType())
				{
					case RTCP::FeedbackRtp::MessageType::NACK:
					{
						auto* nackPacket = dynamic_cast<RTC::RTCP::FeedbackRtpNackPacket*>(packet);

						consumer->ReceiveNack(nackPacket);

						break;
					}

					default:
					{
						MS_WARN_TAG(
						  rtcp,
						  "ignoring unsupported %s Feedback packet "
						  "[sender ssrc:%" PRIu32 ", media ssrc:%" PRIu32 "]",
						  RTCP::FeedbackRtpPacket::MessageType2String(feedback->GetMessageType()).c_str(),
						  feedback->GetMediaSsrc(),
						  feedback->GetMediaSsrc());
					}
				}

				break;
			}

			case RTCP::Type::SR:
			{
				auto* sr = dynamic_cast<RTCP::SenderReportPacket*>(packet);
				auto it  = sr->Begin();

				// Even if Sender Report packet can only contains one report...
				for (; it != sr->End(); ++it)
				{
					auto& report = (*it);
					// Get the producer associated to the SSRC indicated in the report.
					auto* producer = this->rtpListener.GetProducer(report->GetSsrc());

					if (producer == nullptr)
					{
						MS_WARN_TAG(
						  rtcp,
						  "no Producer found for received Sender Report [ssrc:%" PRIu32 "]",
						  report->GetSsrc());

						continue;
					}

					producer->ReceiveRtcpSenderReport(report);
				}

				break;
			}

			case RTCP::Type::SDES:
			{
				auto* sdes = dynamic_cast<RTCP::SdesPacket*>(packet);
				auto it    = sdes->Begin();

				for (; it != sdes->End(); ++it)
				{
					auto& chunk = (*it);
					// Get the producer associated to the SSRC indicated in the report.
					auto* producer = this->rtpListener.GetProducer(chunk->GetSsrc());

					if (producer == nullptr)
					{
						MS_WARN_TAG(
						  rtcp, "no Producer for received SDES chunk [ssrc:%" PRIu32 "]", chunk->GetSsrc());

						continue;
					}
				}

				break;
			}

			case RTCP::Type::BYE:
			{
				MS_DEBUG_TAG(rtcp, "ignoring received RTCP BYE");

				break;
			}

			default:
			{
				MS_WARN_TAG(
				  rtcp,
				  "unhandled RTCP type received [type:%" PRIu8 "]",
				  static_cast<uint8_t>(packet->GetType()));
			}
		}
	}

	void Transport::SendRtcp(uint64_t now)
	{
		MS_TRACE();

		// - Create a CompoundPacket.
		// - Request every Consumer and Producer their RTCP data.
		// - Send the CompoundPacket.

		std::unique_ptr<RTC::RTCP::CompoundPacket> packet(new RTC::RTCP::CompoundPacket());

		for (auto& kv : this->mapConsumers)
		{
			auto* consumer = kv.second;

			consumer->GetRtcp(packet.get(), now);

			// Send the RTCP compound packet if there is a sender report.
			if (packet->HasSenderReport())
			{
				// Ensure that the RTCP packet fits into the RTCP buffer.
				if (packet->GetSize() > RTC::RTCP::BufferSize)
				{
					MS_WARN_TAG(rtcp, "cannot send RTCP packet, size too big (%zu bytes)", packet->GetSize());

					return;
				}

				packet->Serialize(RTC::RTCP::Buffer);
				SendRtcpCompoundPacket(packet.get());

				// Reset the Compound packet.
				packet.reset(new RTC::RTCP::CompoundPacket());
			}
		}

		for (auto& producer : this->producers)
		{
			producer->GetRtcp(packet.get(), now);
		}

		// Send the RTCP compound with all receiver reports.
		if (packet->GetReceiverReportCount() != 0u)
		{
			// Ensure that the RTCP packet fits into the RTCP buffer.
			if (packet->GetSize() > RTC::RTCP::BufferSize)
			{
				MS_WARN_TAG(rtcp, "cannot send RTCP packet, size too big (%zu bytes)", packet->GetSize());

				return;
			}

			packet->Serialize(RTC::RTCP::Buffer);
			SendRtcpCompoundPacket(packet.get());
		}
	}

	inline RTC::Consumer* Transport::GetStartedConsumer(uint32_t ssrc) const
	{
		MS_TRACE();

		for (auto& kv : this->mapConsumers)
		{
			auto* consumer = kv.second;

			// Ignore if not started.
			if (!consumer->IsStarted())
				continue;

			// TODO: I do not like this at all.

			auto& rtpParameters = consumer->GetParameters();

			for (auto& encoding : rtpParameters.encodings)
			{
				if (encoding.ssrc == ssrc)
					return consumer;
				if (encoding.hasRtx && encoding.rtx.ssrc == ssrc)
					return consumer;
				if (encoding.hasFec && encoding.fec.ssrc == ssrc)
					return consumer;
			}
		}

		return nullptr;
	}

	inline void Transport::OnProducerPaused(RTC::Producer* producer)
	{
		this->OnTransportProducerPaused(this, producer);
	}

	inline void Transport::OnProducerResumed(RTC::Producer* producer)
	{
		this->OnTransportProducerResumed(this, producer);
	}

	inline void Transport::OnProducerStreamEnabled(
	  RTC::Producer* producer, const RTC::RtpStream* rtpStream, uint32_t mappedSsrc)
	{
		this->OnTransportProducerStreamEnabled(this, producer, rtpStream, mappedSsrc);
	}

	inline void Transport::OnProducerStreamDisabled(
	  RTC::Producer* producer, const RTC::RtpStream* rtpStream, uint32_t mappedSsrc)
	{
		this->OnTransportProducerStreamDisabled(this, producer, rtpStream, mappedSsrc);
	}

	inline void Transport::OnProducerRtpPacketReceived(RTC::Producer* producer, RTC::RtpPacket* packet)
	{
		this->OnTransportProducerRtpPacketReceived(this, producer, packet);
	}

	inline void Transport::OnProducerSendRtcpPacket(RTC::Producer* /*producer*/, RTC::RTCP::Packet* packet)
	{
		SendRtcpPacket(packet);
	}

	inline void Transport::OnConsumerSendRtpPacket(RTC::Producer* /*consumer*/, RTC::Packet* packet)
	{
		SendRtpPacket(packet);
	}

	inline void Transport::OnConsumerKeyFrameRequired(RTC::Consumer* consumer)
	{
		this->OnTransportConsumerKeyFrameRequested(this, consumer);
	}

	void Transport::OnTimer(Timer* timer)
	{
		if (timer == this->rtcpTimer)
		{
			uint64_t interval = RTC::RTCP::MaxVideoIntervalMs;
			uint64_t now      = DepLibUV::GetTime();

			SendRtcp(now);

			// Recalculate next RTCP interval.
			if (!this->mapConsumers.empty())
			{
				// Transmission rate in kbps.
				uint32_t rate = 0;

				// Get the RTP sending rate.
				for (auto& kv : this->mapConsumers)
				{
					auto* consumer = kv.second;

					rate += consumer->GetTransmissionRate(now) / 1000;
				}

				// Calculate bandwidth: 360 / transmission bandwidth in kbit/s
				if (rate != 0u)
					interval = 360000 / rate;

				if (interval > RTC::RTCP::MaxVideoIntervalMs)
					interval = RTC::RTCP::MaxVideoIntervalMs;
			}

			/*
			 * The interval between RTCP packets is varied randomly over the range
			 * [0.5,1.5] times the calculated interval to avoid unintended synchronization
			 * of all participants.
			 */
			interval *= static_cast<float>(Utils::Crypto::GetRandomUInt(5, 15)) / 10;
			this->rtcpTimer->Start(interval);
		}
	}
} // namespace RTC
