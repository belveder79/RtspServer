// PHZ
// 2018-6-10

#include "RtspConnection.h"
#include "RtspServer.h"
#include "MediaSession.h"
#include "MediaSource.h"
#include "net/SocketUtil.h"

#define USER_AGENT "ar4rtsp" //Lavf57.83.100" //"-_-"
#define RTSP_DEBUG 0
#define MAX_RTSP_MESSAGE_SIZE 2048

using namespace xop;
using namespace std;

RtspConnection::RtspConnection(std::shared_ptr<Rtsp> rtsp, TaskScheduler *task_scheduler, SOCKET sockfd)
	: TcpConnection(task_scheduler, sockfd)
	, rtsp_(rtsp)
	, task_scheduler_(task_scheduler)
	, rtp_channel_(new Channel(sockfd))
	, rtsp_request_(new RtspRequest)
	, rtsp_response_(new RtspResponse)
{
#if RTSP_DEBUG
	#if defined(ANDROID)
			__android_log_print(ANDROID_LOG_ERROR,  MODULE_NAME, "RtspRequest Constructor");
	#else
			std::cout << "RtspConnection Constructor" <<  std::endl;
	#endif
#endif
	this->SetReadCallback([this](std::shared_ptr<TcpConnection> conn, xop::BufferReader& buffer) {
		return this->OnRead(buffer);
	});

	this->SetCloseCallback([this](std::shared_ptr<TcpConnection> conn) {
		this->OnClose();
	});

	alive_count_ = 1;

	rtp_channel_->SetReadCallback([this]() { this->HandleRead(); });
	rtp_channel_->SetWriteCallback([this]() { this->HandleWrite(); });
	rtp_channel_->SetCloseCallback([this]() { this->HandleClose(); });
	rtp_channel_->SetErrorCallback([this]() { this->HandleError(); });

	for(int chn=0; chn<MAX_MEDIA_CHANNEL; chn++) {
		rtcp_channels_[chn] = nullptr;
	}

	has_auth_ = true;
	if (rtsp->authenticator_ != nullptr) {
		has_auth_ = false;
    authenticator_ = rtsp->authenticator_;
	}
}

RtspConnection::~RtspConnection()
{

}

bool RtspConnection::OnRead(BufferReader& buffer)
{
	KeepAlive();

	int size = buffer.ReadableBytes();
	if (size <= 0) {
		return false; //close
	}

	if (conn_mode_ == RTSP_SERVER) {
		if (!HandleRtspRequest(buffer)){
			return false;
		}
	}
	else if (conn_mode_ == RTSP_PUSHER) {
		if (!HandleRtspResponse(buffer)) {
			return false;
		}
	}

	if (buffer.ReadableBytes() > MAX_RTSP_MESSAGE_SIZE) {
		buffer.RetrieveAll();
	}
	return true;
}

void RtspConnection::OnClose()
{
	if(session_id_ != 0) {
		auto rtsp = rtsp_.lock();
		if (rtsp) {
			MediaSession::Ptr media_session = rtsp->LookMediaSession(session_id_);
			if (media_session) {
				media_session->RemoveClient(this->GetSocket());
			}
		}
	}

	for(int chn=0; chn<MAX_MEDIA_CHANNEL; chn++) {
		if(rtcp_channels_[chn] && !rtcp_channels_[chn]->IsNoneEvent()) {
			task_scheduler_->RemoveChannel(rtcp_channels_[chn]);
		}
	}
}

bool RtspConnection::HandleRtspRequest(BufferReader& buffer)
{
#if RTSP_DEBUG
	string str(buffer.Peek(), buffer.ReadableBytes());
	if (str.find("rtsp") != string::npos || str.find("RTSP") != string::npos)
	{
		#if defined(ANDROID)
		  __android_log_print(ANDROID_LOG_VERBOSE,  MODULE_NAME, "RtspConnection::HandleRtspRequest: %s",str.c_str());
		#else
			std::cout << "RtspConnection::HandleRtspRequest: " << str << std::endl;
		#endif
	}
#endif

	if (rtsp_request_->ParseRequest(&buffer)) {
		RtspRequest::Method method = rtsp_request_->GetMethod();
		if(method == RtspRequest::RTCP) {
			HandleRtcp(buffer);
			return true;
		}
		else if(!rtsp_request_->GotAll()) {
			return true;
		}

		switch (method)
		{
			case RtspRequest::OPTIONS:
				HandleCmdOption();
				break;
			case RtspRequest::DESCRIBE:
				HandleCmdDescribe();
				break;
			case RtspRequest::SETUP:
				HandleCmdSetup();
				break;
			case RtspRequest::PLAY:
				HandleCmdPlay();
				break;
			case RtspRequest::TEARDOWN:
				HandleCmdTeardown();
				break;
			case RtspRequest::GET_PARAMETER:
				HandleCmdGetParamter();
				break;
			default:
				break;
		}

		if (rtsp_request_->GotAll()) {
			rtsp_request_->Reset();
		}
	}
	else {
		return false;
	}

	return true;
}

bool RtspConnection::HandleRtspResponse(BufferReader& buffer)
{
#if RTSP_DEBUG
	string str(buffer.Peek(), buffer.ReadableBytes());
	if (str.find("rtsp") != string::npos || str.find("RTSP") != string::npos) {
		#if defined(ANDROID)
			__android_log_print(ANDROID_LOG_VERBOSE,  MODULE_NAME, "RtspConnection::HandleRtspResponse: \n%s",str.c_str());
		#else
			cout << "RtspConnection::HandleRtspResponse: " << endl << str << endl;
		#endif
	}
#endif

    //printf("%s\n", buffer.AsString().c_str());
    if (rtsp_response_->ParseResponse(&buffer)) {
		RtspResponse::Method method = rtsp_response_->GetMethod();
		switch (method)
		{
		case RtspResponse::OPTIONS:
			if (conn_mode_ == RTSP_PUSHER) {
				SendAnnounce();
			}
			break;
		case RtspResponse::ANNOUNCE:
		case RtspResponse::DESCRIBE:
			SendSetup();
			break;
		case RtspResponse::SETUP:
			SendSetup();
			break;
		case RtspResponse::RECORD:
			HandleRecord();
			break;
		default:
			break;
		}
	}
	else {
#if RTSP_DEBUG
		#if defined(ANDROID)
			__android_log_print(ANDROID_LOG_VERBOSE,  MODULE_NAME, "RtspConnection::HandleRtspResponse: ParseResponse failed!");
		#else
			cout << "RtspConnection::HandleRtspResponse: ParseResponse failed!" << endl;
		#endif
#endif
		//=========================================================================
		if (conn_mode_ == RTSP_PUSHER)
		{
            // check if unauthorized
            // if yes, try fix it with authenticator
            // else just return false

            // parse auth info
            if (authenticator_!=nullptr)
            {
                bool unauthorized = false;
                if(authenticator_->HandleUnauthorized(&buffer, _nonce, unauthorized))
                {
                    if(unauthorized)
                    {
#if RTSP_DEBUG
    #if defined(ANDROID)
                        __android_log_print(ANDROID_LOG_VERBOSE,  MODULE_NAME, "RtspConnection::HandleRtspResponse: Resending Options!");
    #else
                        cout << "RtspConnection::HandleRtspResponse: Resending Options!" << endl;
    #endif
#endif
                        rtsp_response_->SetCSeq(rtsp_response_->GetCSeq() + 1);
                        SendOptions(RtspConnection::RTSP_PUSHER);
                        //task_scheduler_->AddTriggerEvent([this]() {
                        //    SendOptions(RtspConnection::RTSP_PUSHER);
                        //});
                        return true;
                    }
                }
            }
		}
		return false;
		//=========================================================================
	}
#if RTSP_DEBUG
	#if defined(ANDROID)
		__android_log_print(ANDROID_LOG_VERBOSE,  MODULE_NAME, "RtspConnection::HandleRtspResponse: ParseResponse ok!");
	#else
		cout << "RtspConnection::HandleRtspResponse: ParseResponse ok!" << endl;
	#endif
#endif
	return true;
}

void RtspConnection::SendRtspMessage(std::shared_ptr<char> buf, uint32_t size)
{
#if RTSP_DEBUG
	#if defined(ANDROID)
		__android_log_print(ANDROID_LOG_VERBOSE,  MODULE_NAME, "RtspConnection::SendRtspMessage: \n%s",buf.get());
	#else
		cout << "RtspConnection::SendRtspMessage: " << endl << buf.get() << endl;
	#endif
#endif

	this->Send(buf, size);
	return;
}

void RtspConnection::HandleRtcp(BufferReader& buffer)
{
	char *peek = buffer.Peek();
	if(peek[0] == '$' &&  buffer.ReadableBytes() > 4) {
		uint32_t pkt_size = peek[2]<<8 | peek[3];
		if(pkt_size +4 >=  buffer.ReadableBytes()) {
			buffer.Retrieve(pkt_size +4);
		}
	}
}

void RtspConnection::HandleRtcp(SOCKET sockfd)
{
	char buf[1024] = {0};
	if(recv(sockfd, buf, 1024, 0) > 0) {
		KeepAlive();
	}
}

void RtspConnection::HandleCmdOption()
{
	std::shared_ptr<char> res(new char[2048], std::default_delete<char[]>());
	int size = rtsp_request_->BuildOptionRes(res.get(), 2048);
	this->SendRtspMessage(res, size);
}

void RtspConnection::HandleCmdDescribe()
{
	if (authenticator_!=nullptr && !HandleAuthentication()) {
		return;
	}

	if (rtp_conn_ == nullptr) {
		rtp_conn_.reset(new RtpConnection(shared_from_this()));
	}

	int size = 0;
	std::shared_ptr<char> res(new char[4096], std::default_delete<char[]>());
	MediaSession::Ptr media_session = nullptr;

	auto rtsp = rtsp_.lock();
	if (rtsp) {
		media_session = rtsp->LookMediaSession(rtsp_request_->GetRtspUrlSession());
	}

	if(!rtsp || !media_session) {
		size = rtsp_request_->BuildNotFoundRes(res.get(), 4096);
	}
	else {
		session_id_ = media_session->GetMediaSessionId();
		media_session->AddClient(this->GetSocket(), rtp_conn_);

		for(int chn=0; chn<MAX_MEDIA_CHANNEL; chn++) {
			MediaSource* source = media_session->GetMediaSource((MediaChannelId)chn);
			if(source != nullptr) {
				rtp_conn_->SetClockRate((MediaChannelId)chn, source->GetClockRate());
				rtp_conn_->SetPayloadType((MediaChannelId)chn, source->GetPayloadType());
			}
		}

		std::string sdp = media_session->GetSdpMessage(SocketUtil::GetSocketIp(this->GetSocket()), rtsp->GetVersion());
		if(sdp == "") {
			size = rtsp_request_->BuildServerErrorRes(res.get(), 4096);
		}
		else {
			size = rtsp_request_->BuildDescribeRes(res.get(), 4096, sdp.c_str());
		}
	}

	SendRtspMessage(res, size);
	return ;
}

void RtspConnection::HandleCmdSetup()
{
	if (authenticator_ != nullptr && !HandleAuthentication()) {
		return;
	}

	int size = 0;
	std::shared_ptr<char> res(new char[4096], std::default_delete<char[]>());
	MediaChannelId channel_id = rtsp_request_->GetChannelId();
	MediaSession::Ptr media_session = nullptr;

	auto rtsp = rtsp_.lock();
	if (rtsp) {
		media_session = rtsp->LookMediaSession(session_id_);
	}

	if(!rtsp || !media_session)  {
		goto server_error;
	}

	if(media_session->IsMulticast())  {
		std::string multicast_ip = media_session->GetMulticastIp();
		if(rtsp_request_->GetTransportMode() == RTP_OVER_MULTICAST) {
			uint16_t port = media_session->GetMulticastPort(channel_id);
			uint16_t session_id = rtp_conn_->GetRtpSessionId();
			if (!rtp_conn_->SetupRtpOverMulticast(channel_id, multicast_ip.c_str(), port)) {
				goto server_error;
			}

			size = rtsp_request_->BuildSetupMulticastRes(res.get(), 4096, multicast_ip.c_str(), port, session_id);
		}
		else {
			goto transport_unsupport;
		}
	}
	else {
		if(rtsp_request_->GetTransportMode() == RTP_OVER_TCP) {
			uint16_t rtp_channel = rtsp_request_->GetRtpChannel();
			uint16_t rtcp_channel = rtsp_request_->GetRtcpChannel();
			uint16_t session_id = rtp_conn_->GetRtpSessionId();

			rtp_conn_->SetupRtpOverTcp(channel_id, rtp_channel, rtcp_channel);
			size = rtsp_request_->BuildSetupTcpRes(res.get(), 4096, rtp_channel, rtcp_channel, session_id);
		}
		else if(rtsp_request_->GetTransportMode() == RTP_OVER_UDP) {
			uint16_t peer_rtp_port = rtsp_request_->GetRtpPort();
			uint16_t peer_rtcp_port = rtsp_request_->GetRtcpPort();
			uint16_t session_id = rtp_conn_->GetRtpSessionId();

			if(rtp_conn_->SetupRtpOverUdp(channel_id, peer_rtp_port, peer_rtcp_port)) {
				SOCKET rtcp_fd = rtp_conn_->GetRtcpSocket(channel_id);
				rtcp_channels_[channel_id].reset(new Channel(rtcp_fd));
				rtcp_channels_[channel_id]->SetReadCallback([rtcp_fd, this]() { this->HandleRtcp(rtcp_fd); });
				rtcp_channels_[channel_id]->EnableReading();
				task_scheduler_->UpdateChannel(rtcp_channels_[channel_id]);
			}
			else {
				goto server_error;
			}

			uint16_t serRtpPort = rtp_conn_->GetRtpPort(channel_id);
			uint16_t serRtcpPort = rtp_conn_->GetRtcpPort(channel_id);
			size = rtsp_request_->BuildSetupUdpRes(res.get(), 4096, serRtpPort, serRtcpPort, session_id);
		}
		else {
			goto transport_unsupport;
		}
	}

	SendRtspMessage(res, size);
	return ;

transport_unsupport:
	size = rtsp_request_->BuildUnsupportedRes(res.get(), 4096);
	SendRtspMessage(res, size);
	return ;

server_error:
	size = rtsp_request_->BuildServerErrorRes(res.get(), 4096);
	SendRtspMessage(res, size);
	return ;
}

void RtspConnection::HandleCmdPlay()
{
	if (authenticator_ != nullptr) {
		if (!HandleAuthentication()) {
			return;
		}
	}

	if (rtp_conn_ == nullptr) {
		return;
	}

	conn_state_ = START_PLAY;
	rtp_conn_->Play();

	uint16_t session_id = rtp_conn_->GetRtpSessionId();
	std::shared_ptr<char> res(new char[2048], std::default_delete<char[]>());

	int size = rtsp_request_->BuildPlayRes(res.get(), 2048, nullptr, session_id);
	SendRtspMessage(res, size);
}

void RtspConnection::HandleCmdTeardown()
{
	if (rtp_conn_ == nullptr) {
		return;
	}

	rtp_conn_->Teardown();

	uint16_t session_id = rtp_conn_->GetRtpSessionId();
	std::shared_ptr<char> res(new char[2048], std::default_delete<char[]>());
	int size = rtsp_request_->BuildTeardownRes(res.get(), 2048, session_id);
	SendRtspMessage(res, size);

	//HandleClose();
}

void RtspConnection::HandleCmdGetParamter()
{
	if (rtp_conn_ == nullptr) {
		return;
	}

	uint16_t session_id = rtp_conn_->GetRtpSessionId();
	std::shared_ptr<char> res(new char[2048], std::default_delete<char[]>());
	int size = rtsp_request_->BuildGetParamterRes(res.get(), 2048, session_id);
	SendRtspMessage(res, size);
}

bool RtspConnection::HandleAuthentication()
{
#if RTSP_DEBUG
	#if defined(ANDROID)
			__android_log_print(ANDROID_LOG_ERROR,  MODULE_NAME, "RtspConnection HandleAuthentication");
	#else
			std::cout << "RtspConnection HandleAuthentication" <<  std::endl;
	#endif
#endif
	if (authenticator_ != nullptr && !has_auth_) {
    if (authenticator_->Authenticate(rtsp_request_, _nonce)) {
			has_auth_ = true;
    } else {
      std::shared_ptr<char> res(new char[4096], std::default_delete<char[]>());
      size_t size = authenticator_->GetFailedResponse(rtsp_request_, res, 4096);
      SendRtspMessage(res, size);
      return false;
    }
	}

	return true;
}

void RtspConnection::SendOptions(ConnectionMode mode)
{
	if (rtp_conn_ == nullptr) {
		rtp_conn_.reset(new RtpConnection(shared_from_this()));
	}

	auto rtsp = rtsp_.lock();
	if (!rtsp) {
		HandleClose();
		return;
	}

	conn_mode_ = mode;
	rtsp_response_->SetUserAgent(USER_AGENT);
	rtsp_response_->SetRtspUrl(rtsp->GetRtspUrl().c_str());

	std::shared_ptr<char> req(new char[2048], std::default_delete<char[]>());
	int size = rtsp_response_->BuildOptionReq(req.get(), 2048, _nonce, authenticator_.get());
#if RTSP_DEBUG
	#if defined(ANDROID)
		__android_log_print(ANDROID_LOG_VERBOSE,  MODULE_NAME, "RtspConnection::SendOptions:");
	#else
		cout << "RtspConnection::SendOptions: " << endl;
	#endif
#endif
	SendRtspMessage(req, size);
}

void RtspConnection::SendTeardown(ConnectionMode mode)
{
    if (rtp_conn_ == nullptr) {
        rtp_conn_.reset(new RtpConnection(shared_from_this()));
    }

    auto rtsp = rtsp_.lock();
    if (!rtsp) {
        HandleClose();
        return;
    }

    conn_mode_ = mode;
    rtsp_response_->SetUserAgent(USER_AGENT);
    rtsp_response_->SetRtspUrl(rtsp->GetRtspUrl().c_str());

    std::shared_ptr<char> req(new char[2048], std::default_delete<char[]>());
    int size = rtsp_response_->BuildTeardownReq(req.get(), 2048, _nonce, authenticator_.get());
#if RTSP_DEBUG
    #if defined(ANDROID)
        __android_log_print(ANDROID_LOG_VERBOSE,  MODULE_NAME, "RtspConnection::SendTeardown:");
    #else
        cout << "RtspConnection::SendTeardown: " << endl;
    #endif
#endif
    SendRtspMessage(req, size);
    rtp_conn_->Teardown();
}

void RtspConnection::SendAnnounce()
{
	MediaSession::Ptr media_session = nullptr;

	auto rtsp = rtsp_.lock();
	if (rtsp) {
		media_session = rtsp->LookMediaSession(1);
	}

	if (!rtsp || !media_session) {
		HandleClose();
		return;
	}
	else {
		session_id_ = media_session->GetMediaSessionId();
		media_session->AddClient(this->GetSocket(), rtp_conn_);

		for (int chn = 0; chn<2; chn++) {
			MediaSource* source = media_session->GetMediaSource((MediaChannelId)chn);
			if (source != nullptr) {
				rtp_conn_->SetClockRate((MediaChannelId)chn, source->GetClockRate());
				rtp_conn_->SetPayloadType((MediaChannelId)chn, source->GetPayloadType());
			}
		}
	}

	std::string sdp = media_session->GetSdpMessage(SocketUtil::GetSocketIp(this->GetSocket()), rtsp->GetVersion());
	if (sdp == "") {
		HandleClose();
		return;
	}

	std::shared_ptr<char> req(new char[4096], std::default_delete<char[]>());
	int size = rtsp_response_->BuildAnnounceReq(req.get(), 4096, sdp.c_str(), _nonce, authenticator_.get());
	SendRtspMessage(req, size);
}

void RtspConnection::SendDescribe()
{
	std::shared_ptr<char> req(new char[2048], std::default_delete<char[]>());
	int size = rtsp_response_->BuildDescribeReq(req.get(), 2048, _nonce, authenticator_.get());
	SendRtspMessage(req, size);
}

void RtspConnection::SendSetup()
{
	int size = 0;
	std::shared_ptr<char> buf(new char[2048], std::default_delete<char[]>());
	MediaSession::Ptr media_session = nullptr;

	auto rtsp = rtsp_.lock();
	if (rtsp) {
		media_session = rtsp->LookMediaSession(session_id_);
	}

	if (!rtsp || !media_session) {
		HandleClose();
		return;
	}

	if (media_session->GetMediaSource(channel_0) && !rtp_conn_->IsSetup(channel_0)) {
        
        if(_udpmode)
        {
            rtp_conn_->SetupRtpOverUdp(channel_0, 0, 1);
            size = rtsp_response_->BuildSetupUdpReq(buf.get(), 2048, channel_0, _appendSessionIdOnSetup, rtp_conn_.get()->GetRtpPort(channel_0), _nonce, authenticator_.get());
        }
        else
        {
            rtp_conn_->SetupRtpOverTcp(channel_0, 0, 1);
            size = rtsp_response_->BuildSetupTcpReq(buf.get(), 2048, channel_0, _appendSessionIdOnSetup, _nonce, authenticator_.get());
        }
	}
	else if (media_session->GetMediaSource(channel_1) && !rtp_conn_->IsSetup(channel_1)) {
        
        if(_udpmode)
        {
            rtp_conn_->SetupRtpOverUdp(channel_1, 2, 3);
            size = rtsp_response_->BuildSetupUdpReq(buf.get(), 2048, channel_1, _appendSessionIdOnSetup, rtp_conn_.get()->GetRtpPort(channel_1), _nonce, authenticator_.get());
        }
        else
        {
            rtp_conn_->SetupRtpOverTcp(channel_1, 2, 3);
            size = rtsp_response_->BuildSetupTcpReq(buf.get(), 2048, channel_1, _appendSessionIdOnSetup, _nonce, authenticator_.get());
        }
	}
	else {
        if(_udpmode)
        {
            if(media_session->GetMediaSource(channel_0) && rtsp_response_.get()->GetRtpFrom() > 0)
                rtp_conn_.get()->SetPeerRtpAddressPort(channel_0, rtp_conn_.get()->GetIp().c_str(), rtsp_response_.get()->GetRtpFrom());
            if(media_session->GetMediaSource(channel_1) && rtsp_response_.get()->GetRtpFrom() > 0)
                rtp_conn_.get()->SetPeerRtpAddressPort(channel_1, rtp_conn_.get()->GetIp().c_str(), rtsp_response_.get()->GetRtpFrom()+1);
        }
		size = rtsp_response_->BuildRecordReq(buf.get(), 2048, _nonce, authenticator_.get());
	}
    // printf("%s\n", buf.get());
	SendRtspMessage(buf, size);
}

void RtspConnection::HandleRecord()
{
	conn_state_ = START_PUSH;
	rtp_conn_->Record();
}
