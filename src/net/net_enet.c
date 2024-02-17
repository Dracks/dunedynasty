/* net.c */

#include <assert.h>
#include <enet/enet.h>
#include <stdio.h>
#include <string.h>

#include "net.h"

#include "client.h"
#include "message.h"
#include "server.h"
#include "../audio/audio.h"
#include "../enhancement.h"
#include "../house.h"
#include "../mods/multiplayer.h"
#include "../newui/chatbox.h"
#include "../newui/menu.h"
#include "../opendune.h"
#include "../pool/pool_house.h"

#if 0
#define NET_LOG(FORMAT,...)	\
	do { fprintf(stderr, "%s:%d " FORMAT "\n", __FUNCTION__, __LINE__, __VA_ARGS__); } while (false)
#else
#define NET_LOG(...)
#endif

char g_net_name[MAX_NAME_LEN + 1] = "Name";
char g_host_addr[MAX_ADDR_LEN + 1] = "0.0.0.0";
char g_host_port[MAX_PORT_LEN + 1] = DEFAULT_PORT_STR;
char g_join_addr[MAX_ADDR_LEN + 1] = "localhost";
char g_join_port[MAX_PORT_LEN + 1] = DEFAULT_PORT_STR;
char g_chat_buf[MAX_CHAT_LEN + 1];

bool g_sendClientList;
bool g_sendScenario;
enum HouseFlag g_client_houses;
enum NetHostType g_host_type;
static ENetHost *s_enet_host;
static ENetPeer *s_enet_peer;

int g_local_client_id;
PeerData g_peer_data[MAX_CLIENTS];

/*--------------------------------------------------------------*/

static PeerData *
Net_NewPeerData(int peerID)
{
	for (int i = 0; i < MAX_CLIENTS; i++) {
		PeerData *data = &g_peer_data[i];

		if (data->id == 0) {
			data->state = CLIENTSTATE_IN_LOBBY;
			data->id = peerID;
			data->name[0] = '\0';
			return data;
		}
	}

	return NULL;
}

static PeerData *
Server_NewClient(void)
{
	static int l_peerID = 0;

	l_peerID = (l_peerID + 1) & 0xFF;

	if (l_peerID == 0)
		l_peerID = 1;

	return Net_NewPeerData(l_peerID);
}

PeerData *
Net_GetPeerData(int peerID)
{
	if (peerID == 0)
		return NULL;

	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (g_peer_data[i].id == peerID)
			return &g_peer_data[i];
	}

	return NULL;
}

const char *
Net_GetClientName(enum HouseType houseID)
{
	const uint8 id = g_multiplayer.client[houseID];
	const PeerData *data = Net_GetPeerData(id);

	return (data != NULL) ? data->name : NULL;
}

enum HouseType
Net_GetClientHouse(int peerID)
{
	if (peerID == 0)
		return HOUSE_INVALID;

	for (enum HouseType h = HOUSE_HARKONNEN; h < HOUSE_NEUTRAL; h++) {
		if (g_multiplayer.client[h] == peerID)
			return h;
	}

	return HOUSE_INVALID;
}

/*--------------------------------------------------------------*/

void
Net_Initialise(void)
{
	enet_initialize();
	atexit(enet_deinitialize);
}

static bool
Net_WaitForEvent(enum _ENetEventType type, enet_uint32 duration)
{
	for (int attempts = duration / 25; attempts > 0; attempts--) {
		ENetEvent event;

		Audio_PollMusic();

		if (enet_host_service(s_enet_host, &event, 25) <= 0)
			continue;

		if (event.type == type)
			return true;
	}

	return false;
}

bool
Server_Send_StartGame(void)
{
	if (!Net_IsPlayable())
		return false;

	Server_Recv_Chat(0, FLAG_HOUSE_ALL, "Game started");

	unsigned char buf[1];
	buf[0] = '1';

	ENetPacket *packet
		= enet_packet_create(buf, sizeof(buf), ENET_PACKET_FLAG_RELIABLE);

	enet_host_broadcast(s_enet_host, 0, packet);
	enet_host_flush(s_enet_host);
	return true;
}

bool
Net_CreateServer(const char *addr, int port, const char *name)
{
	if (g_host_type == HOSTTYPE_NONE && s_enet_host == NULL && s_enet_peer == NULL) {
		/* Currently at most MAX_HOUSE players, or 5 remote clients. */
		const int max_clients = MAX_CLIENTS - 1;

		ENetAddress address;
		enet_address_set_host(&address, addr);
		address.port = port;

		s_enet_host = enet_host_create(&address, max_clients, 2, 0, 0);
		if (s_enet_host == NULL)
			goto error_host_create;

		ChatBox_ClearHistory();
		ChatBox_AddLog(CHATTYPE_LOG, "Server created");

		g_client_houses = 0;
		memset(g_peer_data, 0, sizeof(g_peer_data));
		Multiplayer_Init();

		g_host_type = HOSTTYPE_CLIENT_SERVER;
		PeerData *data = Server_NewClient();
		assert(data != NULL);

		g_local_client_id = data->id;
		Server_Recv_PrefName(g_local_client_id, name);

		return true;
	}

error_host_create:
	return false;
}

bool
Net_ConnectToServer(const char *hostname, int port, const char *name)
{
	if (g_host_type == HOSTTYPE_NONE && s_enet_host == NULL && s_enet_peer == NULL) {
		ENetAddress address;
		enet_address_set_host(&address, hostname);
		address.port = port;

		s_enet_host = enet_host_create(NULL, 1, 2, 57600/8, 14400/8);
		if (s_enet_host == NULL)
			goto error_host_create;

		s_enet_peer = enet_host_connect(s_enet_host, &address, 2, 0);
		if (s_enet_peer == NULL)
			goto error_host_connect;

		if (!Net_WaitForEvent(ENET_EVENT_TYPE_CONNECT, 1000))
			goto error_timeout;

		ChatBox_ClearHistory();
		NET_LOG("Connected to server %s:%d\n", hostname, port);

		memset(g_peer_data, 0, sizeof(g_peer_data));
		Multiplayer_Init();

		enhancement_smooth_unit_animation = SMOOTH_UNIT_ANIMATION_DISABLE;

		g_host_type = HOSTTYPE_DEDICATED_CLIENT;
		g_local_client_id = 0;

		Client_Send_PrefName(name);
		return true;
	}

	goto error_host_create;

error_timeout:
	enet_peer_reset(s_enet_peer);
	s_enet_peer = NULL;

error_host_connect:
	enet_host_destroy(s_enet_host);
	s_enet_host = NULL;

error_host_create:
	return false;
}

void
Net_Disconnect(void)
{
	if (s_enet_host != NULL) {
		if (g_host_type == HOSTTYPE_DEDICATED_SERVER
		 || g_host_type == HOSTTYPE_CLIENT_SERVER) {
			int connected_peers = 0;

			for (int i = 0; i < MAX_CLIENTS; i++) {
				PeerData *data = &g_peer_data[i];

				if (data->peer != NULL) {
					enet_peer_disconnect(data->peer, 0);
					connected_peers++;
				}
			}

			while (connected_peers > 0) {
				if (!Net_WaitForEvent(ENET_EVENT_TYPE_DISCONNECT, 3000))
					break;

				connected_peers--;
			}
		} else if (s_enet_peer != NULL) {
			enet_peer_disconnect(s_enet_peer, 0);
			enet_host_flush(s_enet_host);
		}

		enet_host_destroy(s_enet_host);
		s_enet_host = NULL;
	}

	s_enet_peer = NULL;
	g_host_type = HOSTTYPE_NONE;
}

bool
Net_IsPlayable(void)
{
	return Net_HasAtLeastTwoPlayers() && Net_HasAllPlayersAssigned() && Net_HasAtLeastTwoTeams();
}

bool
Net_HasAtLeastTwoPlayers(void)
{
	int total_players = 0;

	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (g_peer_data[i].state == CLIENTSTATE_IN_LOBBY)
			total_players++;
	}

	return total_players > 1;
}

bool
Net_HasAllPlayersAssigned(void)
{
	int assigned_players = 0;
	int total_players = 0;

	/* XXX - check alliances. */
	for (enum HouseType h = HOUSE_HARKONNEN; h < HOUSE_NEUTRAL; h++) {
		if (g_multiplayer.client[h] != 0)
			assigned_players++;
	}

	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (g_peer_data[i].state == CLIENTSTATE_IN_LOBBY)
			total_players++;
	}

	if (assigned_players > 1 && assigned_players == total_players) {
		return true;
	} else {
		return false;
	}
}

bool
Net_HasAtLeastTwoTeams(void)
{
	int team_count = 0;
	for (enum PlayerTeam t = TEAM_1; t < TEAM_MAX; t++) {
		for (enum HouseType h = HOUSE_HARKONNEN; h < HOUSE_NEUTRAL; h++) {
			if (g_multiplayer.player_config[h].brain != BRAIN_NONE && g_multiplayer.player_config[h].team == t) {
				team_count++;
				break;
			}
		}
	}

	return team_count > 1;
}

bool
Net_HasClientRole(void)
{
	return (g_host_type == HOSTTYPE_CLIENT_SERVER || g_host_type == HOSTTYPE_DEDICATED_CLIENT);
}

bool
Net_HasServerRole(void)
{
	return (g_host_type == HOSTTYPE_DEDICATED_SERVER || g_host_type == HOSTTYPE_CLIENT_SERVER);
}

void
Net_Synchronise(void)
{
	if (g_host_type == HOSTTYPE_NONE)
		return;

	if (g_host_type == HOSTTYPE_CLIENT_SERVER
	 || g_host_type == HOSTTYPE_DEDICATED_CLIENT) {
		assert(g_local_client_id != 0);
		assert(g_playerHouseID != HOUSE_INVALID);
		assert(g_playerHouse != NULL);
	}

	if (g_host_type == HOSTTYPE_DEDICATED_SERVER
	 || g_host_type == HOSTTYPE_CLIENT_SERVER) {
		Server_ResetCache();

		for (enum HouseType h = HOUSE_HARKONNEN; h < HOUSE_NEUTRAL; h++) {
			if (g_multiplayer.client[h] == 0) {
				g_multiplayer.state[h] = MP_HOUSE_UNUSED;
			} else {
				PeerData *data = Net_GetPeerData(g_multiplayer.client[h]);

				data->state = CLIENTSTATE_IN_GAME;
				g_multiplayer.state[h] = MP_HOUSE_PLAYING;

				if (g_multiplayer.client[h] != g_local_client_id)
					g_client_houses |= (1 << h);
			}
		}
	} else {
		Client_ResetCache();
	}

	Multiplayer_GenerateMap(MAP_GENERATOR_FINAL);
}

/*--------------------------------------------------------------*/

void
Net_Send_Chat(const char *msg)
{
	if (g_host_type == HOSTTYPE_DEDICATED_SERVER
	 || g_host_type == HOSTTYPE_CLIENT_SERVER) {
		bool isChat = true;

		if (msg[0] == '/') {
			if (Server_ProcessCommand(msg))
				isChat = false;
		}

		if (isChat)
			Server_Recv_Chat(g_local_client_id, FLAG_HOUSE_ALL, msg);
	} else {
		Client_Send_Chat(msg);
	}
}

void
Server_Recv_Chat(int peerID, enum HouseFlag houses, const char *buf)
{
	const char *name = NULL;
	char msg[2 + MAX_CHAT_LEN + 1];

	msg[0] = '"';
	msg[1] = peerID;
	const int msg_len = snprintf(msg + 2, sizeof(msg) - 2, "%s", buf);

	if (msg_len <= 0)
		return;

	const PeerData *data = Net_GetPeerData(peerID);
	if (data != NULL)
		name = data->name;

	ENetPacket *packet
		= enet_packet_create(msg, 3 + msg_len, ENET_PACKET_FLAG_RELIABLE);

	if (houses == FLAG_HOUSE_ALL) {
		ChatBox_AddChat(peerID, name, msg + 2);
		enet_host_broadcast(s_enet_host, 0, packet);
	} else {
		for (int i = 0; i < MAX_CLIENTS; i++) {
			data = &g_peer_data[i];
			if (data->id == 0)
				continue;

			const enum HouseType houseID = Net_GetClientHouse(data->id);
			if ((houseID == HOUSE_INVALID) || ((houses & (1 << houseID)) != 0))
				continue;

			if (data->id == g_local_client_id) {
				ChatBox_AddChat(peerID, name, msg + 2);
			} else if (data->peer != NULL) {
				enet_peer_send(data->peer, 0, packet);
			}
		}
	}
}

void
Server_SendMessages(void)
{
	if (g_host_type != HOSTTYPE_DEDICATED_SERVER
	 && g_host_type != HOSTTYPE_CLIENT_SERVER)
		return;

	unsigned char *buf = g_server_broadcast_message_buf;

	Server_Send_ClientList(&buf);
	Server_Send_Scenario(&buf);

	if (buf - g_server_broadcast_message_buf > 0) {
		const size_t len = buf - g_server_broadcast_message_buf;

		for (int i = 0; i < MAX_CLIENTS; i++) {
			const PeerData *data = &g_peer_data[i];
			ENetPeer *peer = data->peer;

			if (peer == NULL || Net_GetClientHouse(data->id) != HOUSE_INVALID)
				continue;

			NET_LOG("packet size=%d, num outgoing packets=%lu",
					len, enet_list_size(&peer->outgoingReliableCommands));

			ENetPacket *packet
				= enet_packet_create(g_server_broadcast_message_buf, len,
						ENET_PACKET_FLAG_RELIABLE);

			enet_peer_send(peer, 0, packet);
		}
	}

	Server_Send_UpdateCHOAM(&buf);
	Server_Send_UpdateLandscape(&buf);
	Server_Send_UpdateStructures(&buf);
	Server_Send_UpdateUnits(&buf);
	Server_Send_UpdateExplosions(&buf);

	unsigned char * const buf_start_client_specific = buf;

	for (enum HouseType houseID = HOUSE_HARKONNEN; houseID < HOUSE_NEUTRAL; houseID++) {
		if (g_multiplayer.client[houseID] == 0)
			continue;

		buf = buf_start_client_specific;

		Server_Send_UpdateHouse(houseID, &buf);
		Server_Send_UpdateFogOfWar(houseID, &buf);

		if ((g_server2client_message_len[houseID] > 0)
				&& (buf + g_server2client_message_len[houseID]
					< g_server_broadcast_message_buf + MAX_SERVER_BROADCAST_MESSAGE_LEN)) {
			memcpy(buf, g_server2client_message_buf[houseID],
					g_server2client_message_len[houseID]);
			buf += g_server2client_message_len[houseID];
			g_server2client_message_len[houseID] = 0;
		}

		const int len = buf - g_server_broadcast_message_buf;
		if (len <= 0)
			continue;

		ENetPacket *packet
			= enet_packet_create(g_server_broadcast_message_buf, len,
					ENET_PACKET_FLAG_RELIABLE);

		for (int i = 0; i < MAX_CLIENTS; i++) {
			const PeerData *data = &g_peer_data[i];
			ENetPeer *peer = data->peer;

			if (peer == NULL || Net_GetClientHouse(data->id) != houseID)
				continue;

			NET_LOG("packet size=%d, num outgoing packets=%lu",
					len, enet_list_size(&peer->outgoingReliableCommands));

			enet_peer_send(peer, 0, packet);
		}
	}
}

static void
Server_Send_ClientID(ENetPeer *peer)
{
	unsigned char buf[2];
	unsigned char *p = buf;

	Net_Encode_ServerClientMsg(&p, SCMSG_IDENTITY);
	Net_Encode_uint8(&p, ((const PeerData *) peer->data)->id);
	assert(p - buf == sizeof(buf));

	ENetPacket *packet
		= enet_packet_create(buf, sizeof(buf), ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send(peer, 0, packet);
}

static void
Server_Recv_ConnectClient(ENetEvent *event)
{
	NET_LOG("A new client connected from %x:%u.",
			event->peer->address.host, event->peer->address.port);

	if (g_inGame)
		goto error;

	PeerData *data = Server_NewClient();
	if (data != NULL) {
		event->peer->data = data;
		data->peer = event->peer;

		Server_Send_ClientID(event->peer);
		lobby_map_generator_mode = MAP_GENERATOR_TRY_TEST_ELSE_RAND;
		return;
	}

error:
	enet_peer_disconnect(event->peer, 0);
}

void
Server_DisconnectClient(PeerData *data)
{
	ENetPeer *peer = data->peer;
	char chat_log[MAX_CHAT_LEN + 1];

	snprintf(chat_log, sizeof(chat_log), "%s left", data->name);

	if (data->state == CLIENTSTATE_IN_GAME)
		Server_Recv_ReturnToLobby(Net_GetClientHouse(data->id), false);

	Server_Recv_PrefHouse(data->id, HOUSE_INVALID);
	enet_peer_disconnect(data->peer, 0);
	peer->data = NULL;

	data->state = CLIENTSTATE_UNUSED;
	data->id = 0;
	data->peer = NULL;

	lobby_map_generator_mode = MAP_GENERATOR_TRY_TEST_ELSE_RAND;
	g_sendClientList = true;

	Server_Recv_Chat(0, FLAG_HOUSE_ALL, chat_log);
}

static void
Server_Recv_DisconnectClient(ENetEvent *event)
{
	if (event->peer->data != NULL) {
		PeerData *data = event->peer->data;

		NET_LOG("Disconnect client from %x:%u.",
				event->peer->address.host, event->peer->address.port);

		Server_DisconnectClient(data);
	}
}

void
Server_RecvMessages(void)
{
	if (g_host_type == HOSTTYPE_DEDICATED_CLIENT)
		return;

	/* Process the local player's commands. */
	if (g_host_type == HOSTTYPE_NONE
	 || g_host_type == HOSTTYPE_CLIENT_SERVER) {
		Server_ProcessMessage(g_local_client_id, g_playerHouseID,
				g_client2server_message_buf, g_client2server_message_len);
		g_client2server_message_len = 0;

		if (g_host_type == HOSTTYPE_NONE)
			return;
	}

	ENetEvent event;
	while (enet_host_service(s_enet_host, &event, 0) > 0) {
		switch (event.type) {
			case ENET_EVENT_TYPE_RECEIVE:
				{
					ENetPacket *packet = event.packet;
					const PeerData *data = event.peer->data;
					const enum HouseType houseID = Net_GetClientHouse(data->id);
					Server_ProcessMessage(data->id, houseID,
							packet->data, packet->dataLength);
					enet_packet_destroy(packet);
				}
				break;

			case ENET_EVENT_TYPE_CONNECT:
				Server_Recv_ConnectClient(&event);
				break;

			case ENET_EVENT_TYPE_DISCONNECT:
				Server_Recv_DisconnectClient(&event);
				break;

			case ENET_EVENT_TYPE_NONE:
			default:
				break;
		}
	}
}

void
Client_SendMessages(void)
{
	if (g_host_type != HOSTTYPE_DEDICATED_CLIENT)
		return;

	if (g_client2server_message_len <= 0)
		return;

	NET_LOG("packet size=%d, num outgoing packets=%lu",
			g_client2server_message_len,
			enet_list_size(&s_enet_host->peers[0].outgoingReliableCommands));

	ENetPacket *packet
		= enet_packet_create(
				g_client2server_message_buf, g_client2server_message_len,
				ENET_PACKET_FLAG_RELIABLE);

	enet_peer_send(s_enet_peer, 0, packet);
	g_client2server_message_len = 0;
}

enum NetEvent
Client_RecvMessages(void)
{
	enum NetEvent ret = NETEVENT_NORMAL;

	if (g_host_type == HOSTTYPE_DEDICATED_SERVER) {
		return ret;
	} else if (g_host_type != HOSTTYPE_DEDICATED_CLIENT) {
		House_Client_UpdateRadarState();
		Client_ChangeSelectionMode();
		return ret;
	}

	ENetEvent event;
	while (enet_host_service(s_enet_host, &event, 0) > 0) {
		switch (event.type) {
			case ENET_EVENT_TYPE_RECEIVE:
				{
					ENetPacket *packet = event.packet;
					ret = Client_ProcessMessage(packet->data, packet->dataLength);
					enet_packet_destroy(packet);
				}
				break;

			case ENET_EVENT_TYPE_DISCONNECT:
				Net_Disconnect();
				return NETEVENT_DISCONNECT;

			case ENET_EVENT_TYPE_CONNECT:
			case ENET_EVENT_TYPE_NONE:
			default:
				break;
		}
	}

	return ret;
}
