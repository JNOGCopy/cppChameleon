#include "ClientNetworking.h"

#include <algorithm>
#include <chrono>
#include <iostream>

namespace
{
	constexpr enet_uint16 NET_PULSE_SERVER_PORT = 7769;
	constexpr std::uint32_t PACKET_HEADER_MASK = 0x7FFF'FFFFu;
	constexpr int CONNECT_TIMEOUT_MS = 5000;
	constexpr int CONNECT_POLL_SLICE_MS = 250;

	void destroyClientHost(ClientNetworking &service)
	{
		if (service.serverPeer)
		{
			enet_peer_disconnect_now(service.serverPeer, 0);
			service.serverPeer = nullptr;
		}

		if (service.client)
		{
			enet_host_destroy(service.client);
			service.client = nullptr;
		}
	}

	void setConnectionState(ClientNetworking &service,
		ClientNetworking::ConnectionState state, std::string status)
	{
		service.connectionState = state;
		service.lastStatus = std::move(status);
	}

	ClientNetworking::RoundPhase decodeRoundPhase(std::uint32_t roundPhase)
	{
		switch (roundPhase)
		{
		case roundPhaseHiderHide: return ClientNetworking::RoundPhase::HiderHide;
		case roundPhaseHunterSearch: return ClientNetworking::RoundPhase::HunterSearch;
		default: return ClientNetworking::RoundPhase::Lobby;
		}
	}
}

bool ClientNetworking::connectToServer(const char *serverAddress)
{
	shutdown();

	connectedServerAddress = (serverAddress && serverAddress[0]) ? serverAddress : "localhost";
	receivedPlayerData = false;
	receivedInitialGameState = false;
	localCID = 0;
	gameActive = false;
	hunterCID = 0;
	currentMapIndex = 0;
	roundPhase = RoundPhase::Lobby;
	roundTimerRemainingSeconds = 0.0f;
	localPlayerFound = false;
	roundResult = RoundResult::None;
	remotePlayers.clear();
	remotePaintUpdates.clear();
	foundHiderIDs.clear();

	client = enet_host_create(nullptr, 1, SERVER_CHANNELS, 0, 0);
	if (!client)
	{
		setConnectionState(*this, ConnectionState::Failed, "Failed to create ENet client host.");
		return false;
	}

	ENetAddress address = {};
	address.port = NET_PULSE_SERVER_PORT;
	if (enet_address_set_host(&address, connectedServerAddress.c_str()) != 0)
	{
		destroyClientHost(*this);
		setConnectionState(*this, ConnectionState::Failed, "Invalid server address.");
		return false;
	}

	serverPeer = enet_host_connect(client, &address, SERVER_CHANNELS, 0);
	if (!serverPeer)
	{
		destroyClientHost(*this);
		setConnectionState(*this, ConnectionState::Failed, "Failed to create ENet peer.");
		return false;
	}

	setConnectionState(
		*this,
		ConnectionState::Connecting,
		"Connecting to " + connectedServerAddress + ":" + std::to_string(NET_PULSE_SERVER_PORT) + "...");

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(CONNECT_TIMEOUT_MS);

	while (std::chrono::steady_clock::now() < deadline)
	{
		const auto remainingMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - std::chrono::steady_clock::now()).count();
		const int waitTime = (std::max)(1, (std::min)(CONNECT_POLL_SLICE_MS, static_cast<int>(remainingMilliseconds)));

		ENetEvent event = {};
		const int serviceResult = enet_host_service(client, &event, waitTime);
		if (serviceResult < 0)
		{
			destroyClientHost(*this);
			setConnectionState(*this, ConnectionState::Failed, "Failed while waiting for server connection.");
			return false;
		}

		if (serviceResult == 0)
		{
			continue;
		}

		switch (event.type)
		{
		case ENET_EVENT_TYPE_CONNECT:
		{
			setConnectionState(*this, ConnectionState::Connected, "Connected. Waiting for player data...");
			break;
		}

		case ENET_EVENT_TYPE_RECEIVE:
		{
			receiveDataFromServer(event);
			enet_packet_destroy(event.packet);

			if (receivedPlayerData && receivedInitialGameState)
			{
				return true;
			}

			if (connectionState == ConnectionState::Failed)
			{
				destroyClientHost(*this);
				return false;
			}
			break;
		}

		case ENET_EVENT_TYPE_DISCONNECT:
		{
			serverPeer = nullptr;
			receivedPlayerData = false;
			receivedInitialGameState = false;
			localCID = 0;
			gameActive = false;
			hunterCID = 0;
			currentMapIndex = 0;
			roundPhase = RoundPhase::Lobby;
			roundTimerRemainingSeconds = 0.0f;
			localPlayerFound = false;
			roundResult = RoundResult::None;
			foundHiderIDs.clear();
			destroyClientHost(*this);
			setConnectionState(*this, ConnectionState::Failed, "Disconnected before receiving player data.");
			return false;
		}

		default:
			break;
		}
	}

	destroyClientHost(*this);
	setConnectionState(*this, ConnectionState::Failed, "Timed out waiting for the server handshake.");
	return false;
}

bool ClientNetworking::sendPlayerState(const Packet_PlayerStateUpdate &playerState, bool reliable)
{
	if (!client || !serverPeer || connectionState != ConnectionState::Connected || localCID == 0)
	{
		return false;
	}

	sendPacket(
		serverPeer,
		headerPlayerStateUpdate,
		localCID,
		(void *)&playerState,
		sizeof(playerState),
		reliable,
		CHANNEL_GAMEPLAY);

	return true;
}

bool ClientNetworking::sendHunterHitPlayer(std::uint64_t targetCID)
{
	if (!client || !serverPeer || connectionState != ConnectionState::Connected || localCID == 0 || targetCID == 0)
	{
		return false;
	}

	Packet_HunterHitPlayer hitPacket = {};
	hitPacket.targetCID = targetCID;

	sendPacket(
		serverPeer,
		headerHunterHitPlayer,
		localCID,
		(void *)&hitPacket,
		sizeof(hitPacket),
		true,
		CHANNEL_CONNECTIONS);

	return true;
}

bool ClientNetworking::sendPaintTextureUpdate(int meshIndex, glm::ivec2 size, int quality,
	const std::vector<unsigned char> &pixels)
{
	if (!client || !serverPeer || connectionState != ConnectionState::Connected || localCID == 0)
	{
		return false;
	}

	Packet_PlayerPaintTextureUpdate header = {};
	header.meshIndex = meshIndex;
	header.size = size;
	header.quality = quality;
	header.pixelDataSize = static_cast<std::uint32_t>(pixels.size());

	std::vector<unsigned char> payload(sizeof(header) + pixels.size());
	std::memcpy(payload.data(), &header, sizeof(header));
	if (!pixels.empty())
	{
		std::memcpy(payload.data() + sizeof(header), pixels.data(), pixels.size());
	}

	Packet packet = {};
	packet.header = headerPlayerPaintTextureUpdate;
	packet.cid = localCID;

	sendPacketAndCompress(
		serverPeer,
		packet,
		reinterpret_cast<const char *>(payload.data()),
		payload.size(),
		true,
		CHANNEL_PAINTING);

	return true;
}

void ClientNetworking::update(float deltaTime)
{
	if (!client)
	{
		return;
	}

	int waitTime = connectionState == ConnectionState::Connecting ? 1 : 0;
	int tries = 10;
	ENetEvent event = {};

	while ((enet_host_service(client, &event, waitTime) > 0) || (waitTime = 0, tries-- > 0))
	{
		waitTime = 0;

		switch (event.type)
		{
		case ENET_EVENT_TYPE_CONNECT:
		{
			setConnectionState(*this, ConnectionState::Connected, "Connected. Waiting for player data...");
			break;
		}

		case ENET_EVENT_TYPE_RECEIVE:
		{
			receiveDataFromServer(event);
			enet_packet_destroy(event.packet);
			break;
		}

		case ENET_EVENT_TYPE_DISCONNECT:
		{
			serverPeer = nullptr;
			receivedPlayerData = false;
			receivedInitialGameState = false;
			localCID = 0;
			gameActive = false;
			hunterCID = 0;
			currentMapIndex = 0;
			roundPhase = RoundPhase::Lobby;
			roundTimerRemainingSeconds = 0.0f;
			localPlayerFound = false;
			roundResult = RoundResult::None;
			remotePlayers.clear();
			remotePaintUpdates.clear();
			foundHiderIDs.clear();
			setConnectionState(*this, ConnectionState::Disconnected, "Disconnected from server.");
			break;
		}

		default:
			break;
		}
	}

	if (gameActive && roundPhase != RoundPhase::Lobby && deltaTime > 0.0f)
	{
		roundTimerRemainingSeconds = (std::max)(0.0f, roundTimerRemainingSeconds - deltaTime);
	}
}

void ClientNetworking::shutdown()
{
	destroyClientHost(*this);
	connectionState = ConnectionState::Disconnected;
	lastStatus = "Disconnected";
	localCID = 0;
	receivedPlayerData = false;
	receivedInitialGameState = false;
	gameActive = false;
	hunterCID = 0;
	currentMapIndex = 0;
	roundPhase = RoundPhase::Lobby;
	roundTimerRemainingSeconds = 0.0f;
	localPlayerFound = false;
	roundResult = RoundResult::None;
	remotePlayers.clear();
	remotePaintUpdates.clear();
	foundHiderIDs.clear();
}

void ClientNetworking::receiveDataFromServer(ENetEvent &event)
{
	Packet packet = {};
	size_t packetDataSize = 0;
	char *packetData = parsePacket(event, packet, packetDataSize);
	if (!packetData)
	{
		return;
	}

	char *ownedDecompressedData = nullptr;
	const char *payload = packetData;
	size_t payloadSize = packetDataSize;

	if (packet.isCompressed())
	{
		size_t originalSize = 0;
		ownedDecompressedData = static_cast<char *>(unCompressData(packetData, packetDataSize, originalSize));
		if (!ownedDecompressedData)
		{
			setConnectionState(*this, ConnectionState::Failed, "Failed to decompress packet from server.");
			return;
		}

		payload = ownedDecompressedData;
		payloadSize = originalSize;
	}

	const std::uint32_t header = packet.header & PACKET_HEADER_MASK;
	switch (header)
	{
	case headerReceiveCIDAndData:
	{
		if (payloadSize >= sizeof(Packet_ReceiveCIDAndData))
		{
			const auto &receivedPacket = *reinterpret_cast<const Packet_ReceiveCIDAndData *>(payload);
			localCID = receivedPacket.yourCID;
			receivedPlayerData = true;
			setConnectionState(*this, ConnectionState::Connected, "Received player data from server.");
		}
		break;
	}

	case headerGameStateUpdate:
	{
		if (payloadSize >= sizeof(Packet_GameStateUpdate))
		{
			const bool wasGameActive = gameActive;
			const RoundPhase previousRoundPhase = roundPhase;
			const auto &receivedPacket = *reinterpret_cast<const Packet_GameStateUpdate *>(payload);
			receivedInitialGameState = true;
			const bool mapChanged = currentMapIndex != receivedPacket.currentMapIndex;
			gameActive = receivedPacket.gameActive != 0;
			hunterCID = receivedPacket.hunterCID;
			currentMapIndex = receivedPacket.currentMapIndex;
			roundPhase = gameActive ? decodeRoundPhase(receivedPacket.roundPhase) : RoundPhase::Lobby;
			roundTimerRemainingSeconds = gameActive ? static_cast<float>(receivedPacket.timerSeconds) : 0.0f;

			const bool enteredHidePhase = roundPhase == RoundPhase::HiderHide
				&& (!wasGameActive || previousRoundPhase != RoundPhase::HiderHide);
			if (mapChanged || enteredHidePhase)
			{
				localPlayerFound = false;
				roundResult = RoundResult::None;
				foundHiderIDs.clear();
			}
			else
			{
				localPlayerFound = false;
				foundHiderIDs.clear();
			}

			if (!gameActive)
			{
				lastStatus = "Game ended.";
			}
			else if (roundPhase == RoundPhase::HiderHide)
			{
				lastStatus = "Hide time started. Hunter CID: " + std::to_string(hunterCID) + ".";
			}
			else if (roundPhase == RoundPhase::HunterSearch)
			{
				lastStatus = "Hunter search started. Hunter CID: " + std::to_string(hunterCID) + ".";
			}
			else
			{
				lastStatus = "Game started. Hunter CID: " + std::to_string(hunterCID) + ".";
			}
		}
		break;
	}

	case headerPlayerFoundNotification:
	{
		if (payloadSize >= sizeof(Packet_PlayerFoundNotification))
		{
			const auto &receivedPacket = *reinterpret_cast<const Packet_PlayerFoundNotification *>(payload);
			if (receivedPacket.targetCID != 0 && !isPlayerFound(receivedPacket.targetCID))
			{
				foundHiderIDs.push_back(receivedPacket.targetCID);
			}

			if (receivedPacket.targetCID == localCID)
			{
				localPlayerFound = true;
				lastStatus = "You were hit by the hunter.";
			}
		}
		break;
	}

	case headerRoundResult:
	{
		if (payloadSize >= sizeof(Packet_RoundResult))
		{
			const auto &receivedPacket = *reinterpret_cast<const Packet_RoundResult *>(payload);
			if (receivedPacket.result == roundResultHunterWon)
			{
				roundResult = RoundResult::HunterWon;
				lastStatus = "Hunter won the round.";
			}
		}
		break;
	}

	case headerPlayerStateUpdate:
	{
		if (payloadSize >= sizeof(Packet_PlayerStateUpdate) && packet.cid != 0 && packet.cid != localCID)
		{
			const auto &receivedPacket = *reinterpret_cast<const Packet_PlayerStateUpdate *>(payload);
			auto &remotePlayer = remotePlayers[packet.cid];
			remotePlayer.position = receivedPacket.position;
			remotePlayer.yaw = receivedPacket.yaw;
			remotePlayer.animationIndex = receivedPacket.animationIndex;
		}
		break;
	}

	case headerPlayerPaintTextureUpdate:
	{
		if (packet.cid != 0 && packet.cid != localCID && payloadSize >= sizeof(Packet_PlayerPaintTextureUpdate))
		{
			const auto &receivedHeader = *reinterpret_cast<const Packet_PlayerPaintTextureUpdate *>(payload);
			const size_t pixelBytes = payloadSize - sizeof(Packet_PlayerPaintTextureUpdate);
			if (receivedHeader.meshIndex >= 0
				&& receivedHeader.size.x > 0
				&& receivedHeader.size.y > 0
				&& receivedHeader.pixelDataSize == pixelBytes)
			{
				auto &remotePaint = remotePaintUpdates[packet.cid][receivedHeader.meshIndex];
				remotePaint.meshIndex = receivedHeader.meshIndex;
				remotePaint.size = receivedHeader.size;
				remotePaint.quality = receivedHeader.quality;
				remotePaint.pixels.resize(pixelBytes);
				if (pixelBytes > 0)
				{
					std::memcpy(
						remotePaint.pixels.data(),
						reinterpret_cast<const unsigned char *>(payload) + sizeof(Packet_PlayerPaintTextureUpdate),
						pixelBytes);
				}
			}
		}
		break;
	}

	case headerClientDisconnected:
	{
		if (payloadSize >= sizeof(Packet_ClientDisconnected))
		{
			const auto &receivedPacket = *reinterpret_cast<const Packet_ClientDisconnected *>(payload);
			remotePlayers.erase(receivedPacket.cid);
			remotePaintUpdates.erase(receivedPacket.cid);
			foundHiderIDs.erase(
				std::remove(foundHiderIDs.begin(), foundHiderIDs.end(), receivedPacket.cid),
				foundHiderIDs.end());
		}
		break;
	}

	default:
		lastStatus = "Received unknown packet header " + std::to_string(header) + ".";
		break;
	}

	delete[] ownedDecompressedData;
}

bool ClientNetworking::isHunter(std::uint64_t cid) const
{
	return gameActive && cid != 0 && cid == hunterCID;
}

bool ClientNetworking::isLocalHunter() const
{
	return isHunter(localCID);
}

bool ClientNetworking::isPlayerFound(std::uint64_t cid) const
{
	return std::find(foundHiderIDs.begin(), foundHiderIDs.end(), cid) != foundHiderIDs.end();
}

const char *ClientNetworking::getConnectionStateName() const
{
	switch (connectionState)
	{
	case ConnectionState::Disconnected: return "Disconnected";
	case ConnectionState::Connecting: return "Connecting";
	case ConnectionState::Connected: return "Connected";
	case ConnectionState::Failed: return "Failed";
	default: return "Unknown";
	}
}
