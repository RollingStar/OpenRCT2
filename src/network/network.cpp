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

extern "C" {
#include "../platform/platform.h"
}

#include "network.h"

extern "C" {
#include "../addresses.h"
}

#ifndef DISABLE_NETWORK

#include <math.h>
#include <algorithm>
#include <set>
extern "C" {
#include "../config.h"
#include "../game.h"
#include "../interface/chat.h"
#include "../interface/window.h"
#include "../localisation/date.h"
#include "../localisation/localisation.h"
#include "../scenario.h"
}

#pragma comment(lib, "Ws2_32.lib")

Network gNetwork;

enum {
	NETWORK_SUCCESS,
	NETWORK_NO_DATA,
	NETWORK_MORE_DATA,
	NETWORK_DISCONNECTED
};

enum {
	NETWORK_COMMAND_AUTH,
	NETWORK_COMMAND_MAP,
	NETWORK_COMMAND_CHAT,
	NETWORK_COMMAND_GAMECMD,
	NETWORK_COMMAND_TICK,
	NETWORK_COMMAND_PLAYERLIST,
	NETWORK_COMMAND_PING,
	NETWORK_COMMAND_PINGLIST,
	NETWORK_COMMAND_MAX
};

const char *NetworkCommandNames[] = {
	"NETWORK_COMMAND_AUTH",
	"NETWORK_COMMAND_MAP",
	"NETWORK_COMMAND_CHAT",
	"NETWORK_COMMAND_GAMECMD",
	"NETWORK_COMMAND_TICK",
	"NETWORK_COMMAND_PLAYERLIST",
	"NETWORK_COMMAND_PING",
	"NETWORK_COMMAND_PINGLIST",
};

NetworkPacket::NetworkPacket()
{
	transferred = 0;
	read = 0;
	size = 0;
	data = std::make_shared<std::vector<uint8>>();
}

std::unique_ptr<NetworkPacket> NetworkPacket::Allocate()
{
	return std::unique_ptr<NetworkPacket>(new NetworkPacket); // change to make_unique in c++14
}

std::unique_ptr<NetworkPacket> NetworkPacket::Duplicate(NetworkPacket& packet)
{
	return std::unique_ptr<NetworkPacket>(new NetworkPacket(packet)); // change to make_unique in c++14
}

uint8* NetworkPacket::GetData()
{
	return &(*data)[0];
}

void NetworkPacket::Write(uint8* bytes, unsigned int size)
{
	data->insert(data->end(), bytes, bytes + size);
}

void NetworkPacket::WriteString(const char* string)
{
	Write((uint8*)string, strlen(string) + 1);
}

const uint8* NetworkPacket::Read(unsigned int size)
{
	if (read + size > NetworkPacket::size) {
		return 0;
	} else {
		uint8* data = &GetData()[read];
		read += size;
		return data;
	}
}

const char* NetworkPacket::ReadString()
{
	char* str = (char*)&GetData()[read];
	char* strend = str;
	while (read < size && *strend != 0) {
		read++;
		strend++;
	}
	if (*strend != 0) {
		return 0;
	}
	read++;
	return str;
}

void NetworkPacket::Clear()
{
	transferred = 0;
	read = 0;
	data->clear();
}

NetworkPlayer::NetworkPlayer(const char* name)
{
	strncpy((char*)NetworkPlayer::name, name, sizeof(NetworkPlayer::name));
	NetworkPlayer::name[sizeof(NetworkPlayer::name) - 1] = 0;
	ping = 0;
	flags = 0;
}

NetworkConnection::NetworkConnection()
{
	authstatus = NETWORK_AUTH_NONE;
	player = 0;
}

int NetworkConnection::ReadPacket()
{
 	if (inboundpacket.transferred < sizeof(inboundpacket.size)) {
		// read packet size
		int readBytes = recv(socket, &((char*)&inboundpacket.size)[inboundpacket.transferred], sizeof(inboundpacket.size) - inboundpacket.transferred, 0);
		if (readBytes == SOCKET_ERROR || readBytes == 0) {
			if (LAST_SOCKET_ERROR() != EWOULDBLOCK && LAST_SOCKET_ERROR() != EAGAIN) {
				return NETWORK_DISCONNECTED;
			} else {
				return NETWORK_NO_DATA; 
			}
		}
		inboundpacket.transferred += readBytes;
		if (inboundpacket.transferred == sizeof(inboundpacket.size)) {
			inboundpacket.size = ntohs(inboundpacket.size);
			if(inboundpacket.size == 0){ // Can't have a size 0 packet
				return NETWORK_DISCONNECTED;
			}
			inboundpacket.data->resize(inboundpacket.size);
		}
	} else {
		// read packet data
		if (inboundpacket.data->capacity() > 0) {
			int readBytes = recv(socket, (char*)&inboundpacket.GetData()[inboundpacket.transferred - sizeof(inboundpacket.size)], sizeof(inboundpacket.size) + inboundpacket.size - inboundpacket.transferred, 0);
			if (readBytes == SOCKET_ERROR || readBytes == 0) {
				if (LAST_SOCKET_ERROR() != EWOULDBLOCK && LAST_SOCKET_ERROR() != EAGAIN) {
					return NETWORK_DISCONNECTED;
				} else {
					return NETWORK_NO_DATA;
				}
			}
			inboundpacket.transferred += readBytes;
		}
		if (inboundpacket.transferred == sizeof(inboundpacket.size) + inboundpacket.size) {
			return NETWORK_SUCCESS;
		}
	}
	return NETWORK_MORE_DATA;
}

bool NetworkConnection::SendPacket(NetworkPacket& packet)
{
	uint16 sizen = htons(packet.size);
	std::vector<uint8> tosend;
	tosend.reserve(sizeof(sizen) + packet.size);
	tosend.insert(tosend.end(), (uint8*)&sizen, (uint8*)&sizen + sizeof(sizen));
	tosend.insert(tosend.end(), packet.data->begin(), packet.data->end());
	while (1) {
		int sentBytes = send(socket, (const char*)&tosend[packet.transferred], tosend.size() - packet.transferred, 0);
		if (sentBytes == SOCKET_ERROR) {
			return false;
		}
		packet.transferred += sentBytes;
		if (packet.transferred == tosend.size()) {
			return true;
		}
	}
	return false;
}

void NetworkConnection::QueuePacket(std::unique_ptr<NetworkPacket> packet)
{
	if (authstatus == NETWORK_AUTH_OK || authstatus == NETWORK_AUTH_REQUESTED) {
		packet->size = (uint16)packet->data->size();
		outboundpackets.push_back(std::move(packet));
	}
}

void NetworkConnection::SendQueuedPackets()
{
	while (outboundpackets.size() > 0 && SendPacket(*(outboundpackets.front()).get())) {
		outboundpackets.remove(outboundpackets.front());
	}
}

bool NetworkConnection::SetTCPNoDelay(bool on)
{
	return setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on)) == 0;
}

bool NetworkConnection::SetNonBlocking(bool on)
{
	return SetNonBlocking(socket, on);
}

bool NetworkConnection::SetNonBlocking(SOCKET socket, bool on)
{
#ifdef _WIN32
	u_long nonblocking = on;
	return ioctlsocket(socket, FIONBIO, &nonblocking) == 0;
#else
	int flags = fcntl(socket, F_GETFL, 0);
	return fcntl(socket, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

Network::Network()
{
	wsa_initialized = false;
	mode = NETWORK_MODE_NONE;
	last_tick_sent_time = 0;
	last_ping_sent_time = 0;
	strcpy(password, "");
	client_command_handlers.resize(NETWORK_COMMAND_MAX, 0);
	client_command_handlers[NETWORK_COMMAND_AUTH] = &Network::Client_Handle_AUTH;
	client_command_handlers[NETWORK_COMMAND_MAP] = &Network::Client_Handle_MAP;
	client_command_handlers[NETWORK_COMMAND_CHAT] = &Network::Client_Handle_CHAT;
	client_command_handlers[NETWORK_COMMAND_GAMECMD] = &Network::Client_Handle_GAMECMD;
	client_command_handlers[NETWORK_COMMAND_TICK] = &Network::Client_Handle_TICK;
	client_command_handlers[NETWORK_COMMAND_PLAYERLIST] = &Network::Client_Handle_PLAYERLIST;
	client_command_handlers[NETWORK_COMMAND_PING] = &Network::Client_Handle_PING;
	client_command_handlers[NETWORK_COMMAND_PINGLIST] = &Network::Client_Handle_PINGLIST;
	server_command_handlers.resize(NETWORK_COMMAND_MAX, 0);
	server_command_handlers[NETWORK_COMMAND_AUTH] = &Network::Server_Handle_AUTH;
	server_command_handlers[NETWORK_COMMAND_CHAT] = &Network::Server_Handle_CHAT;
	server_command_handlers[NETWORK_COMMAND_GAMECMD] = &Network::Server_Handle_GAMECMD;
	server_command_handlers[NETWORK_COMMAND_PING] = &Network::Server_Handle_PING;
}

Network::~Network()
{
	Close();
}

bool Network::Init()
{
#ifdef _WIN32
	if (!wsa_initialized) {
		log_verbose("Initialising WSA");
		WSADATA wsa_data;
		if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
			log_error("Unable to initialise winsock.");
			return false;
		}
		wsa_initialized = true;
	}
#endif
	return true;
}

void Network::Close()
{
	if (mode == NETWORK_MODE_CLIENT) {
		closesocket(server_socket);
	} else
	if (mode == NETWORK_MODE_SERVER) {
		closesocket(listening_socket);
		for(auto it = client_connection_list.begin(); it != client_connection_list.end(); it++) {
			closesocket((*it)->socket);
		}
	}

	mode = NETWORK_MODE_NONE;

	client_connection_list.clear();
	game_command_queue.clear();
	player_list.clear();

#ifdef _WIN32
	if (wsa_initialized) {
		WSACleanup();
		wsa_initialized = false;
	}
#endif

	gfx_invalidate_screen();
}

bool Network::BeginClient(const char* host, unsigned short port)
{
	Close();
	if (!Init())
		return false;

	server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server_socket == INVALID_SOCKET) {
		log_error("Unable to create socket.");
		return false;
	}

	sockaddr_in server_address;
#ifdef USE_INET_PTON
	char address[64];
	if (!network_get_address(address, sizeof(address), host)) {
		log_error("Unable to resolve hostname.");
		return false;
	}

	if (inet_pton(AF_INET, address, &server_address.sin_addr) != 1) {
		return false;
	}
#else
	server_address.sin_addr.S_un.S_addr = inet_addr(network_getAddress((char *)host));
#endif // USE_INET_PTON
	server_address.sin_family = AF_INET;
	server_address.sin_port = htons(port);

	if (connect(server_socket, (sockaddr*)&server_address, sizeof(server_address)) != 0) {
		log_error("Unable to connect to host.");
		return false;
	} else {
		printf("Connected to server!\n");
	}

	server_connection.socket = server_socket;
	server_connection.SetTCPNoDelay(true);
	if (!server_connection.SetNonBlocking(true)) {
		closesocket(server_socket);
		log_error("Failed to set non-blocking mode.");
		return false;
	}

	mode = NETWORK_MODE_CLIENT;

	Client_Send_AUTH(OPENRCT2_VERSION, gConfigNetwork.player_name, "");
	return true;
}

bool Network::BeginServer(unsigned short port)
{
	Close();
	if (!Init())
		return false;

	log_verbose("Begin listening for clients");
	listening_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listening_socket == INVALID_SOCKET) {
		log_error("Unable to create socket.");
		return false;
	}

	sockaddr_in local_address;
	local_address.sin_family = AF_INET;
	local_address.sin_addr.s_addr = INADDR_ANY;
	local_address.sin_port = htons(port);

	if (bind(listening_socket, (sockaddr*)&local_address, sizeof(local_address)) != 0) {
		closesocket(listening_socket);
		log_error("Unable to bind to socket.");
		return false;
	}

	if (listen(listening_socket, SOMAXCONN) != 0) {
		closesocket(listening_socket);
		log_error("Unable to listen on socket.");
		return false;
	}

	if (!NetworkConnection::SetNonBlocking(listening_socket, true)) {
		closesocket(listening_socket);
		log_error("Failed to set non-blocking mode.");
		return false;
	}

	NetworkPlayer* player = AddPlayer(gConfigNetwork.player_name);
	player->flags |= NETWORK_PLAYER_FLAG_ISSERVER;
	player_id = player->id;

	printf("Ready for clients...\n");

	mode = NETWORK_MODE_SERVER;
	return true;
}

int Network::GetMode()
{
	return mode;
}

int Network::GetAuthStatus()
{
	if (GetMode() == NETWORK_MODE_CLIENT) {
		return server_connection.authstatus;
	} else
	if (GetMode() == NETWORK_MODE_SERVER) {
		return NETWORK_AUTH_OK;
	}
	return NETWORK_AUTH_NONE;
}

uint32 Network::GetServerTick()
{
	return server_tick;
}

uint8 Network::GetPlayerID()
{
	return player_id;
}

void Network::Update()
{
	switch (GetMode()) {
	case NETWORK_MODE_SERVER:
		UpdateServer();
		break;
	case NETWORK_MODE_CLIENT:
		UpdateClient();
		break;
	}
}

void Network::UpdateServer()
{
	auto it = client_connection_list.begin();
	while (it != client_connection_list.end()) {
		if (!ProcessConnection(*(*it))) {
			RemoveClient((*it));
			it = client_connection_list.begin();
		} else {
			it++;
		}
	}
	if (SDL_GetTicks() - last_tick_sent_time >= 25) {
		last_tick_sent_time = SDL_GetTicks();
		Server_Send_TICK();
	}
	if (SDL_GetTicks() - last_ping_sent_time >= 3000) {
		last_ping_sent_time = SDL_GetTicks();
		Server_Send_PING();
		Server_Send_PINGLIST();
	}
	SOCKET socket = accept(listening_socket, NULL, NULL);
	if (socket == INVALID_SOCKET) {
		if (LAST_SOCKET_ERROR() != EWOULDBLOCK) {
			PrintError();
			log_error("Failed to accept client.");
		}
	} else {
		if (!NetworkConnection::SetNonBlocking(socket, true)) {
			closesocket(socket);
			log_error("Failed to set non-blocking mode.");
		} else {
			AddClient(socket);
		}
	}
}

void Network::UpdateClient()
{
	if (!ProcessConnection(server_connection)) {
		Close();
	}
	ProcessGameCommandQueue();

	// Check synchronisation
	if (!_desynchronised && !CheckSRAND(RCT2_GLOBAL(RCT2_ADDRESS_CURRENT_TICKS, uint32), RCT2_GLOBAL(RCT2_ADDRESS_SCENARIO_SRAND_0, uint32))) {
		_desynchronised = true;
		window_network_status_open("Network desync detected");
		if (!gConfigNetwork.stay_connected) {
			Close();
		}
	}
}

NetworkPlayer* Network::GetPlayerByID(int id) {
	auto it = std::find_if(player_list.begin(), player_list.end(), [&id](std::unique_ptr<NetworkPlayer> const& player) { return player->id == id; });
	if (it != player_list.end()) {
		return (*it).get();
	}
	return 0;
}

const char* Network::FormatChat(NetworkPlayer* fromplayer, const char* text)
{
	static char formatted[1024];
	char* lineCh = formatted;
	formatted[0] = 0;
	if (fromplayer) {
		lineCh = utf8_write_codepoint(lineCh, FORMAT_OUTLINE);
		lineCh = utf8_write_codepoint(lineCh, FORMAT_BABYBLUE);
		strcpy(lineCh, (const char*)fromplayer->name);
		strcat(lineCh, ": ");
	}
	strcat(formatted, text);
	return formatted;
}

void Network::SendPacketToClients(NetworkPacket& packet)
{
	for (auto it = client_connection_list.begin(); it != client_connection_list.end(); it++) {
		(*it)->QueuePacket(std::move(NetworkPacket::Duplicate(packet)));
	}
}

bool Network::CheckSRAND(uint32 tick, uint32 srand0)
{
	if (server_srand0_tick == 0)
		return true;

	if (tick > server_srand0_tick) {
		server_srand0_tick = 0;
		return true;
	}

	if (tick == server_srand0_tick) {
		server_srand0_tick = 0;
		if (srand0 != server_srand0) {
			return false;
		}
	}
	return true;
}

void Network::Client_Send_AUTH(const char* gameversion, const char* name, const char* password)
{
	std::unique_ptr<NetworkPacket> packet = std::move(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_AUTH;
	packet->WriteString(gameversion);
	packet->WriteString(name);
	packet->WriteString(password);
	server_connection.authstatus = NETWORK_AUTH_REQUESTED;
	server_connection.QueuePacket(std::move(packet));
}

void Network::Server_Send_MAP(NetworkConnection* connection)
{
	int buffersize = 0x600000;
	std::vector<uint8> buffer(buffersize);
	SDL_RWops* rw = SDL_RWFromMem(&buffer[0], buffersize);
	scenario_save_network(rw);
	int size = (int)SDL_RWtell(rw);
	int chunksize = 1000;
	for (int i = 0; i < size; i += chunksize) {
		int datasize = (std::min)(chunksize, size - i);
		std::unique_ptr<NetworkPacket> packet = std::move(NetworkPacket::Allocate());
		*packet << (uint32)NETWORK_COMMAND_MAP << (uint32)size << (uint32)i;
		packet->Write(&buffer[i], datasize);
		if (connection) {
			connection->QueuePacket(std::move(packet));
		} else {
			SendPacketToClients(*packet);
		}
	}
	SDL_RWclose(rw);
}

void Network::Client_Send_CHAT(const char* text)
{
	std::unique_ptr<NetworkPacket> packet = std::move(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_CHAT;
	packet->Write((uint8*)text, strlen(text) + 1);
	server_connection.QueuePacket(std::move(packet));
}

void Network::Server_Send_CHAT(const char* text)
{
	std::unique_ptr<NetworkPacket> packet = std::move(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_CHAT;
	packet->Write((uint8*)text, strlen(text) + 1);
	SendPacketToClients(*packet);
}

void Network::Client_Send_GAMECMD(uint32 eax, uint32 ebx, uint32 ecx, uint32 edx, uint32 esi, uint32 edi, uint32 ebp, uint8 callback)
{
	std::unique_ptr<NetworkPacket> packet = std::move(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_GAMECMD << (uint32)RCT2_GLOBAL(RCT2_ADDRESS_CURRENT_TICKS, uint32) << eax << (ebx | GAME_COMMAND_FLAG_NETWORKED) << ecx << edx << esi << edi << ebp << callback; 
	server_connection.QueuePacket(std::move(packet));
}

void Network::Server_Send_GAMECMD(uint32 eax, uint32 ebx, uint32 ecx, uint32 edx, uint32 esi, uint32 edi, uint32 ebp, uint8 playerid, uint8 callback)
{
	std::unique_ptr<NetworkPacket> packet = std::move(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_GAMECMD << (uint32)RCT2_GLOBAL(RCT2_ADDRESS_CURRENT_TICKS, uint32) << eax << (ebx | GAME_COMMAND_FLAG_NETWORKED) << ecx << edx << esi << edi << ebp << playerid << callback; 
	SendPacketToClients(*packet);
}

void Network::Server_Send_TICK()
{
	std::unique_ptr<NetworkPacket> packet = std::move(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_TICK << (uint32)RCT2_GLOBAL(RCT2_ADDRESS_CURRENT_TICKS, uint32) << (uint32)RCT2_GLOBAL(RCT2_ADDRESS_SCENARIO_SRAND_0, uint32);
	SendPacketToClients(*packet);
}

void Network::Server_Send_PLAYERLIST()
{
	std::unique_ptr<NetworkPacket> packet = std::move(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_PLAYERLIST << (uint32)player_list.size();
	for (unsigned int i = 0; i < player_list.size(); i++) {
		packet->WriteString((const char*)player_list[i]->name);
		*packet << player_list[i]->id << player_list[i]->flags;
	}
	SendPacketToClients(*packet);
}

void Network::Client_Send_PING()
{
	std::unique_ptr<NetworkPacket> packet = std::move(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_PING;
	server_connection.QueuePacket(std::move(packet));
}

void Network::Server_Send_PING()
{
	std::unique_ptr<NetworkPacket> packet = std::move(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_PING;
	for (auto it = client_connection_list.begin(); it != client_connection_list.end(); it++) {
		(*it)->ping_time = SDL_GetTicks();
	}
	SendPacketToClients(*packet);
}

void Network::Server_Send_PINGLIST()
{
	std::unique_ptr<NetworkPacket> packet = std::move(NetworkPacket::Allocate());
	*packet << (uint32)NETWORK_COMMAND_PINGLIST << (uint32)player_list.size();
	for (unsigned int i = 0; i < player_list.size(); i++) {
		*packet << player_list[i]->id << player_list[i]->ping;
	}
	SendPacketToClients(*packet);
}

bool Network::ProcessConnection(NetworkConnection& connection)
{
	int packetStatus;
	do {
		packetStatus = connection.ReadPacket();
		switch(packetStatus) {
		case NETWORK_DISCONNECTED:
			// closed connection or network error
			PrintError();
			if (GetMode() == NETWORK_MODE_CLIENT) {
				printf("Server disconnected...\n");
				return false;
			} else
			if (GetMode() == NETWORK_MODE_SERVER) {
				printf("Client disconnected...\n");
				return false;
			}
			break;
		case NETWORK_SUCCESS:
			// done reading in packet
			ProcessPacket(connection, connection.inboundpacket);
			break;
		case NETWORK_MORE_DATA:
			// more data required to be read
			break;
		case NETWORK_NO_DATA:
			// could not read anything from socket
			break;
		}
	} while (packetStatus == NETWORK_MORE_DATA || packetStatus == NETWORK_SUCCESS);
	connection.SendQueuedPackets();
	return true;
}

void Network::ProcessPacket(NetworkConnection& connection, NetworkPacket& packet)
{
	uint32 command;
	packet >> command;
	if (command < NETWORK_COMMAND_MAX) {
		// printf("RECV %s\n", NetworkCommandNames[command]);
		switch (gNetwork.GetMode()) {
		case NETWORK_MODE_SERVER:
			if (server_command_handlers[command]) {
				if (connection.authstatus == NETWORK_AUTH_OK || command == NETWORK_COMMAND_AUTH) {
					(this->*server_command_handlers[command])(connection, packet);
				}
			}
			break;
		case NETWORK_MODE_CLIENT:
			if (client_command_handlers[command]) {
				(this->*client_command_handlers[command])(connection, packet);
			}
			break;
		}
	}
	packet.Clear();
}

void Network::ProcessGameCommandQueue()
{
	while (game_command_queue.begin() != game_command_queue.end() && game_command_queue.begin()->tick == RCT2_GLOBAL(RCT2_ADDRESS_CURRENT_TICKS, uint32)) {
		// run all the game commands at the current tick
		const GameCommand& gc = (*game_command_queue.begin());
		if (GetPlayerID() == gc.playerid) {
			game_command_callback = game_command_callback_get_callback(gc.callback);
		}
		game_do_command_p(gc.esi, (int*)&gc.eax, (int*)&gc.ebx, (int*)&gc.ecx, (int*)&gc.edx, (int*)&gc.esi, (int*)&gc.edi, (int*)&gc.ebp);
		game_command_queue.erase(game_command_queue.begin());
	}
}

void Network::AddClient(SOCKET socket)
{
	auto connection = std::unique_ptr<NetworkConnection>(new NetworkConnection);  // change to make_unique in c++14
	connection->socket = socket;
	connection->SetTCPNoDelay(true);
	client_connection_list.push_back(std::move(connection));
}

void Network::RemoveClient(std::unique_ptr<NetworkConnection>& connection)
{
	NetworkPlayer* connection_player = connection->player;
	if (connection_player) {
		char text[256];
		char* lineCh = text;
		lineCh = utf8_write_codepoint(lineCh, FORMAT_OUTLINE);
		lineCh = utf8_write_codepoint(lineCh, FORMAT_RED);
		sprintf(lineCh, "%s has disconnected", connection_player->name);
		chat_history_add(text);
		gNetwork.Server_Send_CHAT(text);
	}
	player_list.erase(std::remove_if(player_list.begin(), player_list.end(), [connection_player](std::unique_ptr<NetworkPlayer>& player){ return player.get() == connection_player; }), player_list.end());
	client_connection_list.remove(connection);
	Server_Send_PLAYERLIST();
}

NetworkPlayer* Network::AddPlayer(const char* name)
{
	NetworkPlayer* addedplayer = 0;
	int newid = -1;
	if (GetMode() == NETWORK_MODE_SERVER) {
		// Find first unused player id
		for (int id = 0; id < 255; id++) {
			if (std::find_if(player_list.begin(), player_list.end(), [&id](std::unique_ptr<NetworkPlayer> const& player) { return player->id == id; }) == player_list.end()) {
				newid = id;
				break;
			}
		}
	} else {
		newid = 0;
	}
	if (newid != -1) {
		std::unique_ptr<NetworkPlayer> player(new NetworkPlayer(name)); // change to make_unique in c++14
		player->id = newid;
		addedplayer = player.get();
		player_list.push_back(std::move(player));
		if (GetMode() == NETWORK_MODE_SERVER) {
			Server_Send_PLAYERLIST();
		}
	}
	return addedplayer;
}

void Network::PrintError()
{
#ifdef _WIN32
	wchar_t *s = NULL;
	FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, LAST_SOCKET_ERROR(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)&s, 0, NULL);
	fprintf(stderr, "%S\n", s);
	LocalFree(s);
#else
	char *s = strerror(LAST_SOCKET_ERROR());
	fprintf(stderr, "%s\n", s);
#endif
	
}

int Network::Client_Handle_AUTH(NetworkConnection& connection, NetworkPacket& packet)
{
	packet >> (uint32&)connection.authstatus >> (uint8&)player_id;
	return 1;
}

int Network::Server_Handle_AUTH(NetworkConnection& connection, NetworkPacket& packet)
{
	if (connection.authstatus != NETWORK_AUTH_OK) {
		const char* gameversion = packet.ReadString();
		const char* name = packet.ReadString();
		const char* password = packet.ReadString();
		uint8 playerid = 0;
		if (!gameversion || strcmp(gameversion, OPENRCT2_VERSION) != 0) {
			connection.authstatus = NETWORK_AUTH_BADVERSION;
		} else
		if (!name) {
			connection.authstatus = NETWORK_AUTH_BADNAME;
		} else
		if (!password || strcmp(password, Network::password) != 0) {
			connection.authstatus = NETWORK_AUTH_BADPASSWORD;
		} else {
			connection.authstatus = NETWORK_AUTH_OK;
			NetworkPlayer* player = AddPlayer(name);
			connection.player = player;
			if (player) {
				playerid = player->id;
				char text[256];
				char* lineCh = text;
				lineCh = utf8_write_codepoint(lineCh, FORMAT_OUTLINE);
				lineCh = utf8_write_codepoint(lineCh, FORMAT_GREEN);
				sprintf(lineCh, "%s has joined the game", player->name);
				chat_history_add(text);
				gNetwork.Server_Send_CHAT(text);
				Server_Send_MAP(&connection);
			}
		}
		std::unique_ptr<NetworkPacket> responsepacket = std::move(NetworkPacket::Allocate());
		*responsepacket << (uint32)NETWORK_COMMAND_AUTH << (uint32)connection.authstatus << (uint8)playerid;
		connection.QueuePacket(std::move(responsepacket));
	}
	return 1;
}

int Network::Client_Handle_MAP(NetworkConnection& connection, NetworkPacket& packet)
{
	uint32 size, offset;
	packet >> size >> offset;
	if (offset > 0x600000) {
		// too big
		return 0;
	} else {
		int chunksize = packet.size - packet.read;
		if (chunksize <= 0) {
			return 0;
		}
		if (offset + chunksize > chunk_buffer.size()) {
			chunk_buffer.resize(offset + chunksize);
		}
		char status[256];
		sprintf(status, "Downloading map ... (%lu / %lu)", (offset + chunksize) / 1000, size / 1000);
		window_network_status_open(status);
		memcpy(&chunk_buffer[offset], (void*)packet.Read(chunksize), chunksize);
		if (offset + chunksize == size) {
			window_network_status_close();

			SDL_RWops* rw = SDL_RWFromMem(&chunk_buffer[0], size);
			if (game_load_network(rw)) {
				game_load_init();
				game_command_queue.clear();
				server_tick = RCT2_GLOBAL(RCT2_ADDRESS_CURRENT_TICKS, uint32);
				server_srand0_tick = 0;
				// window_network_status_open("Loaded new map from network");
				_desynchronised = false;
			}
			SDL_RWclose(rw);
		}
	}
	return 1;
}

int Network::Client_Handle_CHAT(NetworkConnection& connection, NetworkPacket& packet)
{
	const char* text = (char*)packet.Read(packet.size - packet.read);
	if (text) {
		chat_history_add(text);
	}
	return 1;
}

int Network::Server_Handle_CHAT(NetworkConnection& connection, NetworkPacket& packet)
{
	const char* text = (const char*)packet.Read(packet.size - packet.read);
	if (text) {
		const char* formatted = FormatChat(connection.player, text);
		chat_history_add(formatted);
		Server_Send_CHAT(formatted);
	}
	return 1;
}

int Network::Client_Handle_GAMECMD(NetworkConnection& connection, NetworkPacket& packet)
{
	uint32 tick;
	uint32 args[7];
	uint8 playerid;
	uint8 callback;
	packet >> tick >> args[0] >> args[1] >> args[2] >> args[3] >> args[4] >> args[5] >> args[6] >> playerid >> callback;

	GameCommand gc = GameCommand(tick, args, playerid, callback);
	game_command_queue.insert(gc);
	return 1;
}

int Network::Server_Handle_GAMECMD(NetworkConnection& connection, NetworkPacket& packet)
{
	uint32 tick;
	uint32 args[7];
	uint8 playerid;
	uint8 callback;
	if (connection.player) {
		playerid = connection.player->id;
	}
	packet >> tick >> args[0] >> args[1] >> args[2] >> args[3] >> args[4] >> args[5] >> args[6] >> callback;
	Server_Send_GAMECMD(args[0], args[1], args[2], args[3], args[4], args[5], args[6], playerid, callback);
	game_do_command(args[0], args[1], args[2], args[3], args[4], args[5], args[6]);
	return 1;
}

int Network::Client_Handle_TICK(NetworkConnection& connection, NetworkPacket& packet)
{
	uint32 srand0;
	packet >> server_tick >> srand0;
	if (server_srand0_tick == 0) {
		server_srand0 = srand0;
		server_srand0_tick = server_tick;
	}
	return 1;
}

int Network::Client_Handle_PLAYERLIST(NetworkConnection& connection, NetworkPacket& packet)
{
	player_list.clear();
	uint32 size;
	packet >> size;
	for (unsigned int i = 0; i < size; i++) {
		const char* name = packet.ReadString();
		NetworkPlayer* player = AddPlayer(name);
		if (player) {
			packet >> player->id >> player->flags;
			if (player->flags & NETWORK_PLAYER_FLAG_ISSERVER) {
				server_connection.player = player;
			}
		}
	}
	return 1;
}

int Network::Client_Handle_PING(NetworkConnection& connection, NetworkPacket& packet)
{
	Client_Send_PING();
	return 1;
}

int Network::Server_Handle_PING(NetworkConnection& connection, NetworkPacket& packet)
{
	int ping = SDL_GetTicks() - connection.ping_time;
	if (ping < 0) {
		ping = 0;
	}
	if (connection.player) {
		connection.player->ping = ping;
	}
	return 1;
}

int Network::Client_Handle_PINGLIST(NetworkConnection& connection, NetworkPacket& packet)
{
	uint32 size;
	packet >> size;
	for (unsigned int i = 0; i < size; i++) {
		uint8 id;
		uint16 ping;
		packet >> id >> ping;
		NetworkPlayer* player = GetPlayerByID(id);
		if (player) {
			player->ping = ping;
		}
	}
	return 1;
}

int network_init()
{
	return gNetwork.Init();
}

void network_close()
{
	gNetwork.Close();
}

int network_begin_client(const char *host, int port)
{
	if (gNetwork.GetMode() == NETWORK_MODE_NONE) {
	return gNetwork.BeginClient(host, port);
	} else {
		return false;
	}
}

int network_begin_server(int port)
{
	return gNetwork.BeginServer(port);
}

void network_update()
{
	gNetwork.Update();
}

int network_get_mode()
{
	return gNetwork.GetMode();
}

int network_get_authstatus()
{
	return gNetwork.GetAuthStatus();
}

uint32 network_get_server_tick()
{
	return gNetwork.GetServerTick();
}

uint8 network_get_player_id()
{
	return gNetwork.GetPlayerID();
}

int network_get_num_players()
{
	return gNetwork.player_list.size();
}

const char* network_get_player_name(unsigned int index)
{
	return (const char*)gNetwork.player_list[index]->name;
}

uint32 network_get_player_flags(unsigned int index)
{
	return gNetwork.player_list[index]->flags;
}

int network_get_player_ping(unsigned int index)
{
	return gNetwork.player_list[index]->ping;
}

void network_send_map()
{
	gNetwork.Server_Send_MAP();
}

void network_send_chat(const char* text)
{
	if (gNetwork.GetMode() == NETWORK_MODE_CLIENT) {
		gNetwork.Client_Send_CHAT(text);
	} else
	if (gNetwork.GetMode() == NETWORK_MODE_SERVER) {
		NetworkPlayer* player = gNetwork.GetPlayerByID(gNetwork.GetPlayerID());
		const char* formatted = gNetwork.FormatChat(player, text);
		chat_history_add(formatted);
		gNetwork.Server_Send_CHAT(formatted);
	}
}

void network_send_gamecmd(uint32 eax, uint32 ebx, uint32 ecx, uint32 edx, uint32 esi, uint32 edi, uint32 ebp, uint8 callback)
{
	switch (gNetwork.GetMode()) {
	case NETWORK_MODE_SERVER:
		gNetwork.Server_Send_GAMECMD(eax, ebx, ecx, edx, esi, edi, ebp, gNetwork.GetPlayerID(), callback);
		break;
	case NETWORK_MODE_CLIENT:
		gNetwork.Client_Send_GAMECMD(eax, ebx, ecx, edx, esi, edi, ebp, callback);
		break;
	}
}

#ifdef USE_INET_PTON
static bool network_get_address(char *dst, size_t dstLength, const char *host)
{
	struct addrinfo *remoteHost;

	if (getaddrinfo(host, NULL, NULL, &remoteHost) != 0) {
		// Failed to resolve host name
		return false;
	}

	for (; remoteHost != NULL; remoteHost = remoteHost->ai_next) {
		if (remoteHost->ai_family != AF_INET) continue;

		struct sockaddr_in *ipv4SockAddr = (struct sockaddr_in*)remoteHost->ai_addr;
		return inet_ntop(AF_INET, (void*)&ipv4SockAddr->sin_addr, dst, dstLength) != NULL;
	}

	// No IPv4 addresses found for host name
	return false;
}
#else
static char *network_getAddress(char *host)
{
	struct hostent *remoteHost;
	struct in_addr addr;
	remoteHost = gethostbyname(host);
	if (remoteHost != NULL && remoteHost->h_addrtype == AF_INET && remoteHost->h_addr_list[0] != 0) {
		addr.s_addr = *(u_long *)remoteHost->h_addr_list[0];
		return inet_ntoa(addr);
	}

	return host;
}
#endif // USE_INET_PTON


#else
int network_get_mode() { return NETWORK_MODE_NONE; }
uint32 network_get_server_tick() { return RCT2_GLOBAL(RCT2_ADDRESS_CURRENT_TICKS, uint32); }
void network_send_gamecmd(uint32 eax, uint32 ebx, uint32 ecx, uint32 edx, uint32 esi, uint32 edi, uint32 ebp, uint8 callback) {}
void network_send_map() {}
void network_update() {}
int network_begin_client(const char *host, int port) { return 1; }
int network_begin_server(int port) { return 1; }
int network_get_num_players() { return 1; }
const char* network_get_player_name(unsigned int index) { return "local (OpenRCT2 compiled without MP)"; }
uint32 network_get_player_flags(unsigned int index) { return 0; }
int network_get_player_ping(unsigned int index) { return 0; }
void network_send_chat(const char* text) {}
void network_close() {}
#endif /* DISABLE_NETWORK */
