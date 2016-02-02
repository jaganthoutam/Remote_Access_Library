#include "stdafx.h"
#include "ServerNetworkDriver.h"
#include "Image.h"
#include "Shapes.h"
#include "Packet.h"
#include "TCPSocket.h"
#include "IServerDriver.h"
#include "Server_Config.h"
#include "WebSocketListener.h"
#include "IO_Runner.h"
#include "ThreadPool.h"
#include "TCPListener.h"
#include "HttpListener.h"
#include "turbojpeg.h"

namespace SL {
	namespace Remote_Access_Library {
		namespace Network {

			class ServerNetworkDriverImpl : public IBaseNetworkDriver {


				std::shared_ptr<Network::TCPListener> _TCPListener;
				std::shared_ptr<Network::WebSocketListener> _WebSocketListener;
				std::unique_ptr<HttpListener> _HttptListener;

				std::unique_ptr<IO_Runner> _IO_Runner;
				Utilities::ThreadPool _SendPool, _ReceivePool;
				IServerDriver* _IServerDriver;

				std::vector<std::shared_ptr<Network::ISocket>> _Clients;
				std::mutex _ClientsLock;
				Server_Config _Config;
				std::vector<char> _CompressBuffer;

			public:
				ServerNetworkDriverImpl(Server_Config& config, IServerDriver* svrd) : _IServerDriver(svrd), _Config(config) {


				}
				virtual ~ServerNetworkDriverImpl() {
					Stop();
				}
				virtual void OnConnect(const std::shared_ptr<ISocket>& socket) override {
					_IServerDriver->OnConnect(socket);
					std::lock_guard<std::mutex> lock(_ClientsLock);
					_Clients.push_back(socket);
				}
				virtual void OnClose(const std::shared_ptr<Network::ISocket>& socket)override {
					_IServerDriver->OnClose(socket);
					std::lock_guard<std::mutex> lock(_ClientsLock);
					_Clients.erase(std::remove_if(begin(_Clients), end(_Clients), [socket](const std::shared_ptr<Network::ISocket>& p) { return p == socket; }), _Clients.end());
				}

				virtual void OnReceive(const std::shared_ptr<Network::ISocket>& socket, std::shared_ptr<Packet>& p) override
				{
					_IServerDriver->OnReceive(socket, p);//pass up the chain
				}


				std::vector<std::shared_ptr<Network::ISocket>> GetClients() {
					std::lock_guard<std::mutex> lock(_ClientsLock);
					return _Clients;
				}

				void Send(ISocket* socket, Utilities::Rect& r, const Utilities::Image & img) {

					socket->send(ExtractImageRect(r, img));
				}
				void Send(Utilities::Rect& r, const Utilities::Image & img) {
					SendToAll(ExtractImageRect(r, img));
				}

				void SendToAll(std::shared_ptr<Network::Packet>& packet) {
					for (auto& c : GetClients()) {
						c->send(packet);
					}
				}
				void SendToAll(std::vector<std::shared_ptr<Network::Packet>>& packets) {
					for (auto& c : GetClients()) {
						for (auto& p : packets) {
							c->send(p);
						}
					}
				}

				void Start() {
					Stop();

					_IO_Runner = std::make_unique<IO_Runner>();
					_TCPListener = Network::TCPListener::Create(_Config.TCPListenPort, _IO_Runner->get_io_service(), [=](void* socket) {
						SL::Remote_Access_Library::Network::TCPSocket::Create(this, socket);
					});

					if (_Config.WebSocketListenPort > 0) {
						_HttptListener = std::make_unique<HttpListener>(this, _IO_Runner->get_io_service());
						//	_WebSocketListener = Network::WebSocketListener::CreateListener(_Config.WebSocketListenPort, this);
					}


					_HttptListener->Start();
					_TCPListener->Start();
					//_WebSocketListener->Start();
					int k = 0;
				}
				void Stop() {
					_IO_Runner.reset();
					{
						std::lock_guard<std::mutex> lock(_ClientsLock);
						_Clients.clear();//destroy all clients
					}
					_HttptListener.reset();
					_TCPListener.reset();//destroy the listener
					_WebSocketListener.reset();
				}
				void ExtractImageRect(Utilities::Rect r, const Utilities::Image & img, std::vector<char>& outbuffer) {

					auto srcbuf = outbuffer.data();
					for (auto row = r.Origin.Y; row < r.bottom(); row++) {

						auto startcopy = img.data() + (row*img.Stride()*img.Width());//advance rows
						startcopy += r.Origin.X*img.Stride();//advance columns
						memcpy(srcbuf, startcopy, r.Width*img.Stride());
						srcbuf += r.Width*img.Stride();
					}

				}
				std::shared_ptr<Packet> ExtractImageRect(SL::Remote_Access_Library::Utilities::Rect& r, const SL::Remote_Access_Library::Utilities::Image & img) {
					auto compfree = [](void* handle) {tjDestroy(handle); };
					auto _jpegCompressor(std::unique_ptr<void, decltype(compfree)>(tjInitCompress(), compfree));
					auto set = TJSAMP_420;
					auto maxsize = tjBufSize(r.Width, r.Height, TJSAMP_420);
					auto _jpegSize = maxsize;

					_CompressBuffer.reserve(r.Width* r.Height*Utilities::Image::DefaultStride());
					ExtractImageRect(r, img, _CompressBuffer);

					auto srcbuf = (unsigned char*)_CompressBuffer.data();
					PacketHeader header;
					header.Packet_Type = static_cast<unsigned int>(Commands::PACKET_TYPES::IMAGEDIF);
					header.PayloadLen = sizeof(Utilities::Rect) + maxsize;
					auto p = Packet::CreatePacket(header);
					auto dst = (unsigned char*)p->data();
					memcpy(dst, &r, sizeof(Utilities::Rect));
					dst += sizeof(Utilities::Rect);


					if (tjCompress2(_jpegCompressor.get(), srcbuf, r.Width, 0, r.Height, TJPF_BGRX, &dst, &_jpegSize, set, 70, TJFLAG_FASTDCT | TJFLAG_NOREALLOC) == -1) {
						std::cout<<"Err msg "<< tjGetErrorStr()<<std::endl;
					}
					p->header()->PayloadLen = sizeof(Utilities::Rect) + _jpegSize;//adjust the correct size
					return p;
				}
			};
		}
	}
}

SL::Remote_Access_Library::Network::ServerNetworkDriver::ServerNetworkDriver(Network::IServerDriver * r, Server_Config& config) : _ServerNetworkDriverImpl(std::make_unique<ServerNetworkDriverImpl>(config, r))
{

}
SL::Remote_Access_Library::Network::ServerNetworkDriver::~ServerNetworkDriver()
{
	_ServerNetworkDriverImpl.reset();
}


void SL::Remote_Access_Library::Network::ServerNetworkDriver::StartNetworkProcessing()
{
	_ServerNetworkDriverImpl->Start();
}

void SL::Remote_Access_Library::Network::ServerNetworkDriver::StopNetworkProcessing()
{
	_ServerNetworkDriverImpl->Stop();
}

void SL::Remote_Access_Library::Network::ServerNetworkDriver::Send(Utilities::Rect & r, const Utilities::Image & img)
{
	_ServerNetworkDriverImpl->Send(r, img);
}
void SL::Remote_Access_Library::Network::ServerNetworkDriver::Send(ISocket * socket, Utilities::Rect & r, const Utilities::Image & img)
{
	_ServerNetworkDriverImpl->Send(socket, r, img);
}
std::vector<std::shared_ptr<SL::Remote_Access_Library::Network::ISocket>> SL::Remote_Access_Library::Network::ServerNetworkDriver::GetClients()
{
	return _ServerNetworkDriverImpl->GetClients();
}
