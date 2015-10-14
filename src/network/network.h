/*****************************************************************************
 * Copyright (c) 2014 Ted John
 * OpenRCT2, an open source clone of Roller Coaster Tycoon 2.
 *
 * This file is part of OpenRCT2.
 *
 * OpenRCT2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *****************************************************************************/

#ifndef _NETWORK_H_
#define _NETWORK_H_

enum {
	NETWORK_MODE_NONE,
	NETWORK_MODE_CLIENT,
	NETWORK_MODE_SERVER
};

enum {
	NETWORK_PLAYER_FLAG_ISSERVER = 1 << 0,
};

#define NETWORK_DEFAULT_PORT 11753

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus
#include "../common.h"
#include "../platform/platform.h"
#ifdef __cplusplus
}
#endif // __cplusplus

#ifndef DISABLE_NETWORK

#ifndef __MINGW32__
#define USE_INET_PTON
#else
#warning using deprecated network functions in lieu of inet_pton, inet_ntop
#endif // __MINGW32__

#ifdef _WIN32
	#include <winsock2.h>
	#ifdef USE_INET_PTON
		#include <ws2tcpip.h>
	#endif
	#define LAST_SOCKET_ERROR() WSAGetLastError()
	#undef EWOULDBLOCK
	#define EWOULDBLOCK WSAEWOULDBLOCK
#else
	#include <arpa/inet.h>
	#include <netdb.h>
	#include <netinet/tcp.h>
	#include <sys/socket.h>
	#include <sys/fcntl.h>
	typedef int SOCKET;
	#define SOCKET_ERROR -1
	#define INVALID_SOCKET -1
	#define LAST_SOCKET_ERROR() errno
	#define closesocket close
	#define ioctlsocket ioctl
#endif // _WIN32

enum {
	NETWORK_AUTH_NONE,
	NETWORK_AUTH_REQUESTED,
	NETWORK_AUTH_OK,
	NETWORK_AUTH_BADVERSION,
	NETWORK_AUTH_BADNAME,
	NETWORK_AUTH_BADPASSWORD
};

#ifdef __cplusplus

#include <list>
#include <set>
#include <memory>
#include <vector>
#include <SDL.h>

template <std::size_t size>
struct ByteSwapT { };
template <>
struct ByteSwapT<1> { static uint8 SwapBE(uint8 value) { return value; } };
template <>
struct ByteSwapT<2> { static uint16 SwapBE(uint16 value) { return SDL_SwapBE16(value); } };
template <>
struct ByteSwapT<4> { static uint32 SwapBE(uint32 value) { return SDL_SwapBE32(value); } };
template <typename T>
T ByteSwapBE(const T& value) { return ByteSwapT<sizeof(T)>::SwapBE(value); }

class NetworkPacket
{
public:
	NetworkPacket();
	static std::unique_ptr<NetworkPacket> Allocate();
	static std::unique_ptr<NetworkPacket> Duplicate(NetworkPacket& packet);
	uint8* GetData();
	template <typename T>
	NetworkPacket& operator<<(T value) { T swapped = ByteSwapBE(value); uint8* bytes = (uint8*)&swapped; data->insert(data->end(), bytes, bytes + sizeof(value)); return *this; }
	void Write(uint8* bytes, unsigned int size);
	void WriteString(const char* string);
	template <typename T>
	NetworkPacket& operator>>(T& value) { if (read + sizeof(value) > size) { value = 0; } else { value = ByteSwapBE(*((T*)&GetData()[read])); read += sizeof(value); } return *this; }
	const uint8* Read(unsigned int size);
	const char* ReadString();
	void Clear();

	uint16 size;
	std::shared_ptr<std::vector<uint8>> data;
	int transferred;
	int read;
};

class NetworkPlayer
{
public:
	NetworkPlayer(const char* name);
	uint8 id;
	uint8 name[32 + 1];
	uint16 ping;
	uint32 flags;
};

class NetworkConnection
{
public:
	NetworkConnection();
	int ReadPacket();
	void QueuePacket(std::unique_ptr<NetworkPacket> packet);
	void SendQueuedPackets();
	bool SetTCPNoDelay(bool on);
	bool SetNonBlocking(bool on);
	static bool SetNonBlocking(SOCKET socket, bool on);

	SOCKET socket;
	NetworkPacket inboundpacket;
	int authstatus;
	NetworkPlayer* player;
	uint32 ping_time;

private:
	bool SendPacket(NetworkPacket& packet);
	std::list<std::unique_ptr<NetworkPacket>> outboundpackets;
};

class Network
{
public:
	Network();
	~Network();
	bool Init();
	void Close();
	bool BeginClient(const char* host, unsigned short port);
	bool BeginServer(unsigned short port);
	int GetMode();
	int GetAuthStatus();
	uint32 GetServerTick();
	uint8 GetPlayerID();
	void Update();
	NetworkPlayer* GetPlayerByID(int id);
	const char* FormatChat(NetworkPlayer* fromplayer, const char* text);
	void SendPacketToClients(NetworkPacket& packet);
	bool CheckSRAND(uint32 tick, uint32 srand0);

	void Client_Send_AUTH(const char* gameversion, const char* name, const char* password);
	void Server_Send_MAP(NetworkConnection* connection = nullptr);
	void Client_Send_CHAT(const char* text);
	void Server_Send_CHAT(const char* text);
	void Client_Send_GAMECMD(uint32 eax, uint32 ebx, uint32 ecx, uint32 edx, uint32 esi, uint32 edi, uint32 ebp, uint8 callback);
	void Server_Send_GAMECMD(uint32 eax, uint32 ebx, uint32 ecx, uint32 edx, uint32 esi, uint32 edi, uint32 ebp, uint8 playerid, uint8 callback);
	void Server_Send_TICK();
	void Server_Send_PLAYERLIST();
	void Client_Send_PING();
	void Server_Send_PING();
	void Server_Send_PINGLIST();

	std::vector<std::unique_ptr<NetworkPlayer>> player_list;

private:
	bool ProcessConnection(NetworkConnection& connection);
	void ProcessPacket(NetworkConnection& connection, NetworkPacket& packet);
	void ProcessGameCommandQueue();
	void AddClient(SOCKET socket);
	void RemoveClient(std::unique_ptr<NetworkConnection>& connection);
	NetworkPlayer* AddPlayer(const char* name);
	void PrintError();

	struct GameCommand
	{
		GameCommand(uint32 t, uint32* args, uint8 p, uint8 cb) { tick = t, eax = args[0], ebx = args[1], ecx = args[2], edx = args[3], esi = args[4], edi = args[5], ebp = args[6]; playerid = p; callback = cb; };
		uint32 tick;
		uint32 eax, ebx, ecx, edx, esi, edi, ebp;
		uint8 playerid;
		uint8 callback;
		bool operator<(const GameCommand& comp) const {
			return tick < comp.tick;
		}
	};

	int mode;
	bool wsa_initialized;
	SOCKET server_socket;
	SOCKET listening_socket;
	NetworkConnection server_connection;
	uint32 last_tick_sent_time;
	uint32 last_ping_sent_time;
	uint32 server_tick;
	uint32 server_srand0;
	uint32 server_srand0_tick;
	uint8 player_id;
	std::list<std::unique_ptr<NetworkConnection>> client_connection_list;
	std::multiset<GameCommand> game_command_queue;
	std::vector<uint8> chunk_buffer;
	char password[33];
	bool _desynchronised;

	void UpdateServer();
	void UpdateClient();

private:
	std::vector<int (Network::*)(NetworkConnection& connection, NetworkPacket& packet)> client_command_handlers;
	std::vector<int (Network::*)(NetworkConnection& connection, NetworkPacket& packet)> server_command_handlers;
	int Client_Handle_AUTH(NetworkConnection& connection, NetworkPacket& packet);
	int Server_Handle_AUTH(NetworkConnection& connection, NetworkPacket& packet);
	int Client_Handle_MAP(NetworkConnection& connection, NetworkPacket& packet);
	int Client_Handle_CHAT(NetworkConnection& connection, NetworkPacket& packet);
	int Server_Handle_CHAT(NetworkConnection& connection, NetworkPacket& packet);
	int Client_Handle_GAMECMD(NetworkConnection& connection, NetworkPacket& packet);
	int Server_Handle_GAMECMD(NetworkConnection& connection, NetworkPacket& packet);
	int Client_Handle_TICK(NetworkConnection& connection, NetworkPacket& packet);
	int Client_Handle_PLAYERLIST(NetworkConnection& connection, NetworkPacket& packet);
	int Client_Handle_PING(NetworkConnection& connection, NetworkPacket& packet);
	int Server_Handle_PING(NetworkConnection& connection, NetworkPacket& packet);
	int Client_Handle_PINGLIST(NetworkConnection& connection, NetworkPacket& packet);
};

#endif // __cplusplus
#endif /* DISABLE_NETWORK */

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus
int network_init();
void network_close();
int network_begin_client(const char *host, int port);
int network_begin_server(int port);

int network_get_mode();
void network_update();
int network_get_authstatus();
uint32 network_get_server_tick();
uint8 network_get_player_id();
int network_get_num_players();
const char* network_get_player_name(unsigned int index);
uint32 network_get_player_flags(unsigned int index);
int network_get_player_ping(unsigned int index);

void network_send_map();
void network_send_chat(const char* text);
void network_send_gamecmd(uint32 eax, uint32 ebx, uint32 ecx, uint32 edx, uint32 esi, uint32 edi, uint32 ebp, uint8 callback);

void network_print_error();
#ifdef USE_INET_PTON
static bool network_get_address(char *dst, size_t dstLength, const char *host);
#else
static char *network_getAddress(char *host);
#endif // USE_INET_PTON

#ifdef __cplusplus
}
#endif // __cplusplus

#endif
