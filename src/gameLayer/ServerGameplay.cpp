#include <ServerGameplay.h>
#include <iostream>

#include <algorithm>
#include <cmath>

namespace
{
	std::uint64_t getPeerCid(ENetPeer *peer)
	{
		return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(peer->data));
	}

	std::uint32_t clampTimerSeconds(int seconds)
	{
		return static_cast<std::uint32_t>((std::max)(0, seconds));
	}

	std::uint32_t getRoundTimerSecondsRemaining(const ServerGameplay &serverGameplay)
	{
		if (!serverGameplay.gameActive
			|| serverGameplay.roundPhase == roundPhaseLobby
			|| !serverGameplay.roundPhaseTimerRunning)
		{
			return 0;
		}

		const auto now = std::chrono::steady_clock::now();
		if (now >= serverGameplay.roundPhaseDeadline)
		{
			return 0;
		}

		const float remainingSeconds = std::chrono::duration<float>(
			serverGameplay.roundPhaseDeadline - now).count();
		return static_cast<std::uint32_t>(std::ceil((std::max)(0.0f, remainingSeconds)));
	}

	const char *getRoundPhaseLabel(std::uint32_t roundPhase)
	{
		switch (roundPhase)
		{
		case roundPhaseHiderHide: return "Hider Hide";
		case roundPhaseHunterSearch: return "Hunter Search";
		default: return "Lobby";
		}
	}

	void setRoundPhaseState(ServerGameplay &serverGameplay, std::uint32_t roundPhase, std::uint32_t durationSeconds)
	{
		serverGameplay.roundPhase = roundPhase;

		if (serverGameplay.gameActive && roundPhase != roundPhaseLobby)
		{
			serverGameplay.roundPhaseTimerRunning = true;
			serverGameplay.roundPhaseDeadline = std::chrono::steady_clock::now()
				+ std::chrono::seconds(durationSeconds);
		}
		else
		{
			serverGameplay.roundPhaseTimerRunning = false;
			serverGameplay.roundPhaseDeadline = {};
		}
	}

	Packet_GameStateUpdate buildGameStateUpdatePacket(const ServerGameplay &serverGameplay)
	{
		Packet_GameStateUpdate packet = {};
		packet.gameActive = serverGameplay.gameActive ? 1u : 0u;
		packet.hunterCID = serverGameplay.hunterCID;
		packet.roundPhase = serverGameplay.roundPhase;
		packet.timerSeconds = getRoundTimerSecondsRemaining(serverGameplay);
		return packet;
	}

	void broadcastGameStateUpdate(ServerGameplay &serverGameplay)
	{
		if (!serverGameplay.server)
		{
			return;
		}

		const Packet_GameStateUpdate gameState = buildGameStateUpdatePacket(serverGameplay);
		Packet packet = {};
		packet.header = headerGameStateUpdate;
		serverGameplay.broadcastPacketToAllClients(
			packet,
			reinterpret_cast<const char *>(&gameState),
			sizeof(gameState),
			true,
			CHANNEL_CONNECTIONS);
		enet_host_flush(serverGameplay.server);
	}
}

bool ServerGameplay::init()
{
	close();
	playerIDs = 1;
	connectedClients = 0;
	gameActive = false;
	hunterCID = 0;
	roundPhase = roundPhaseLobby;
	roundPhaseDeadline = {};
	roundPhaseTimerRunning = false;
	connectedClientIDs.clear();
	resetRoundProgress();
	lastStatus = "Starting server...";

	ENetAddress adress;
	adress.host = ENET_HOST_ANY;
	adress.port = 7769;

	server = enet_host_create(&adress, 32, SERVER_CHANNELS, 0, 0);
	if (!server)
	{
		lastStatus = "Failed to create ENet server host.";
		return false;
	}

	lastStatus = "Server running on port 7769.";
	return true;
}

void ServerGameplay::update()
{
	if (!server)
	{
		return;
	}

	int waitTime = 1;
	int tries = 10;
	ENetEvent event = {};

	while ((enet_host_service(server, &event, waitTime) > 0) || (waitTime = 0, tries-- > 0))
	{
		waitTime = 0;

		switch (event.type)
		{
		case ENET_EVENT_TYPE_CONNECT:
		{
			addConnection(event);
			std::cout << "Connection event received.\n";
			break;
		}

		case ENET_EVENT_TYPE_RECEIVE:
		{
			recieveDataFromClients(event);
			enet_packet_destroy(event.packet);
			break;
		}

		case ENET_EVENT_TYPE_DISCONNECT:
		{
			std::cout << "disconnect from server: "
				<< event.peer->address.host << " "
				<< event.peer->address.port << "\n\n";
			removeConnection(event);
			break;
		}

		default:
			break;
		}
	}

	if (gameActive && roundPhaseTimerRunning && getCurrentRoundTimerSeconds() == 0)
	{
		if (roundPhase == roundPhaseHiderHide)
		{
			setRoundPhaseState(*this, roundPhaseHunterSearch, clampTimerSeconds(hunterTimerSeconds));
			broadcastGameStateUpdate(*this);
			lastStatus = "Hide time ended. Hunter released.";
		}
		else if (roundPhase == roundPhaseHunterSearch)
		{
			endGame();
			lastStatus = "Hunter timer ended. Game reset to lobby.";
		}
	}
}

void ServerGameplay::close()
{
	if (server)
	{
		enet_host_destroy(server);
		server = nullptr;
	}

	connectedClients = 0;
	gameActive = false;
	hunterCID = 0;
	roundPhase = roundPhaseLobby;
	roundPhaseDeadline = {};
	roundPhaseTimerRunning = false;
	connectedClientIDs.clear();
	resetRoundProgress();
	lastStatus = "Server stopped.";
}

std::uint64_t ServerGameplay::getIdAndIncrement()
{
	return playerIDs++;
}

void ServerGameplay::addConnection(ENetEvent &event)
{
	if (gameActive)
	{
		lastStatus = "Rejected a new client because a game is already running.";
		event.peer->data = nullptr;
		enet_peer_disconnect_now(event.peer, 0);
		return;
	}

	connectedClients++;

	event.peer->timeoutMinimum = 10'000;
	event.peer->timeoutMaximum = 30'000;
	event.peer->timeoutLimit = 64;

	const std::uint64_t id = getIdAndIncrement();
	event.peer->data = reinterpret_cast<void *>(static_cast<std::uintptr_t>(id));
	connectedClientIDs.push_back(id);

	if (hunterCID == 0)
	{
		hunterCID = id;
	}

	Packet packet = {};
	packet.header = headerReceiveCIDAndData;
	packet.cid = id;

	Packet_ReceiveCIDAndData packetToSend = {};
	packetToSend.yourCID = id;

	sendPacket(
		event.peer,
		packet,
		reinterpret_cast<const char *>(&packetToSend),
		sizeof(packetToSend),
		true,
		CHANNEL_CONNECTIONS);
	syncGameStateToPeer(event.peer);
	enet_host_flush(server);

	lastStatus = "Client joined. Total clients: " + std::to_string(connectedClients) + ".";
}

void ServerGameplay::recieveDataFromClients(ENetEvent &event)
{
	Packet packet = {};
	size_t payloadSize = 0;
	char *payload = parsePacket(event, packet, payloadSize);
	if (!payload)
	{
		return;
	}

	const std::uint32_t header = packet.header & 0x7FFF'FFFFu;
	const std::uint64_t cid = getPeerCid(event.peer);
	const bool reliable = (event.packet->flags & ENET_PACKET_FLAG_RELIABLE) != 0;

	switch (header)
	{
	case headerHunterHitPlayer:
	{
		if (payloadSize >= sizeof(Packet_HunterHitPlayer) && gameActive && cid == hunterCID)
		{
			const auto &hitPacket = *reinterpret_cast<const Packet_HunterHitPlayer *>(payload);
			const std::uint64_t targetCID = hitPacket.targetCID;
			if (targetCID != 0
				&& targetCID != hunterCID
				&& hasConnectedClient(targetCID)
				&& !isHiderFound(targetCID))
			{
				foundHiderIDs.push_back(targetCID);

				Packet_PlayerFoundNotification foundNotification = {};
				foundNotification.hunterCID = hunterCID;
				foundNotification.targetCID = targetCID;

				Packet foundPacket = {};
				foundPacket.header = headerPlayerFoundNotification;
				foundPacket.cid = hunterCID;
				broadcastPacketToAllClients(
					foundPacket,
					reinterpret_cast<const char *>(&foundNotification),
					sizeof(foundNotification),
					true,
					CHANNEL_CONNECTIONS);

				lastStatus = "Hunter found player CID " + std::to_string(targetCID) + ".";

				if (getRemainingHiderCount() == 0)
				{
					Packet_RoundResult roundResult = {};
					roundResult.result = roundResultHunterWon;
					roundResult.winnerCID = hunterCID;

					Packet roundResultPacket = {};
					roundResultPacket.header = headerRoundResult;
					roundResultPacket.cid = hunterCID;
					broadcastPacketToAllClients(
						roundResultPacket,
						reinterpret_cast<const char *>(&roundResult),
						sizeof(roundResult),
						true,
						CHANNEL_CONNECTIONS);

					endGame();
					lastStatus = "Hunter won. All hiders were found.";
				}
			}
		}
		break;
	}

	case headerPlayerStateUpdate:
	{
		if (payloadSize >= sizeof(Packet_PlayerStateUpdate) && cid != 0)
		{
			packet.header = headerPlayerStateUpdate;
			packet.cid = cid;
			broadcastPacketToOtherClients(
				event.peer,
				packet,
				payload,
				sizeof(Packet_PlayerStateUpdate),
				reliable,
				CHANNEL_GAMEPLAY);
		}
		break;
	}

	case headerPlayerPaintTextureUpdate:
	{
		if (payloadSize >= sizeof(Packet_PlayerPaintTextureUpdate) && cid != 0)
		{
			packet.cid = cid;
			broadcastPacketToOtherClients(event.peer, packet, payload, payloadSize, true, CHANNEL_PAINTING);
		}
		break;
	}

	default:
		break;
	}
}

void ServerGameplay::removeConnection(ENetEvent &event)
{
	const std::uint64_t cid = getPeerCid(event.peer);
	bool gameEndedBecauseHunterLeft = false;
	event.peer->data = nullptr;

	if (cid != 0)
	{
		connectedClientIDs.erase(
			std::remove(connectedClientIDs.begin(), connectedClientIDs.end(), cid),
			connectedClientIDs.end());
	}

	if (connectedClients > 0)
	{
		connectedClients--;
	}

	if (cid != 0)
	{
		foundHiderIDs.erase(
			std::remove(foundHiderIDs.begin(), foundHiderIDs.end(), cid),
			foundHiderIDs.end());

		Packet packet = {};
		packet.header = headerClientDisconnected;
		packet.cid = cid;

		Packet_ClientDisconnected disconnectedPacket = {};
		disconnectedPacket.cid = cid;

		broadcastPacketToOtherClients(
			event.peer,
			packet,
			reinterpret_cast<const char *>(&disconnectedPacket),
			sizeof(disconnectedPacket),
			true,
			CHANNEL_CONNECTIONS);
	}

	if (cid != 0 && hunterCID == cid)
	{
		if (gameActive)
		{
			endGame();
			gameEndedBecauseHunterLeft = true;
			lastStatus = "Hunter disconnected. Game ended.";
		}
		else
		{
			hunterCID = connectedClientIDs.empty() ? 0 : connectedClientIDs.front();
		}
	}
	else if (connectedClientIDs.empty())
	{
		endGame();
		hunterCID = 0;
	}

	if (!gameActive && !gameEndedBecauseHunterLeft)
	{
		lastStatus = connectedClients == 0
			? "No clients connected."
			: "Client disconnected. Total clients: " + std::to_string(connectedClients) + ".";
	}
}

bool ServerGameplay::startGame(std::uint64_t requestedHunterCID)
{
	if (!server)
	{
		lastStatus = "Server is not running.";
		return false;
	}

	if (connectedClientIDs.empty())
	{
		lastStatus = "Need at least one connected client before starting a game.";
		return false;
	}

	if (!setHunterCID(requestedHunterCID))
	{
		hunterCID = connectedClientIDs.front();
	}

	resetRoundProgress();
	gameActive = true;
	setRoundPhaseState(*this, roundPhaseHiderHide, clampTimerSeconds(hiderTimerSeconds));
	broadcastGameStateUpdate(*this);

	lastStatus = "Game started. Hunter CID: " + std::to_string(hunterCID)
		+ ". Hide time: " + std::to_string(hiderTimerSeconds) + "s.";
	return true;
}

void ServerGameplay::endGame()
{
	if (!server)
	{
		gameActive = false;
		roundPhase = roundPhaseLobby;
		roundPhaseDeadline = {};
		roundPhaseTimerRunning = false;
		resetRoundProgress();
		return;
	}

	const bool wasActive = gameActive || roundPhase != roundPhaseLobby;
	gameActive = false;
	setRoundPhaseState(*this, roundPhaseLobby, 0);

	if (wasActive)
	{
		broadcastGameStateUpdate(*this);
	}

	resetRoundProgress();

	if (!connectedClientIDs.empty())
	{
		lastStatus = "Game ended.";
	}
}

bool ServerGameplay::setHunterCID(std::uint64_t requestedHunterCID)
{
	if (connectedClientIDs.empty())
	{
		hunterCID = 0;
		lastStatus = "No connected clients to select as hunter.";
		return false;
	}

	const std::uint64_t resolvedHunterCID = hasConnectedClient(requestedHunterCID)
		? requestedHunterCID
		: connectedClientIDs.front();
	const bool requestedCIDWasValid = resolvedHunterCID == requestedHunterCID;

	hunterCID = resolvedHunterCID;

	if (gameActive && server)
	{
		broadcastGameStateUpdate(*this);
		lastStatus = "Hunter changed to CID " + std::to_string(hunterCID) + ".";
	}
	else
	{
		lastStatus = "Selected hunter CID: " + std::to_string(hunterCID) + ".";
	}

	return requestedCIDWasValid;
}

bool ServerGameplay::hasConnectedClient(std::uint64_t cid) const
{
	return std::find(connectedClientIDs.begin(), connectedClientIDs.end(), cid) != connectedClientIDs.end();
}

bool ServerGameplay::isHiderFound(std::uint64_t cid) const
{
	return std::find(foundHiderIDs.begin(), foundHiderIDs.end(), cid) != foundHiderIDs.end();
}

std::size_t ServerGameplay::getRemainingHiderCount() const
{
	std::size_t remainingHiders = 0;
	for (std::uint64_t cid : connectedClientIDs)
	{
		if (cid != 0 && cid != hunterCID && !isHiderFound(cid))
		{
			remainingHiders++;
		}
	}

	return remainingHiders;
}

std::uint32_t ServerGameplay::getCurrentRoundTimerSeconds() const
{
	return getRoundTimerSecondsRemaining(*this);
}

const char *ServerGameplay::getRoundPhaseName() const
{
	return getRoundPhaseLabel(roundPhase);
}

void ServerGameplay::resetRoundProgress()
{
	foundHiderIDs.clear();
}

ENetPeer *ServerGameplay::findPeerByCid(std::uint64_t cid)
{
	if (!server || cid == 0)
	{
		return nullptr;
	}

	for (size_t i = 0; i < server->peerCount; ++i)
	{
		ENetPeer &peer = server->peers[i];
		if (peer.state == ENET_PEER_STATE_CONNECTED && getPeerCid(&peer) == cid)
		{
			return &peer;
		}
	}

	return nullptr;
}

void ServerGameplay::syncGameStateToPeer(ENetPeer *peer)
{
	if (!peer)
	{
		return;
	}

	const Packet_GameStateUpdate gameState = buildGameStateUpdatePacket(*this);
	Packet packet = {};
	packet.header = headerGameStateUpdate;
	sendPacket(
		peer,
		packet,
		reinterpret_cast<const char *>(&gameState),
		sizeof(gameState),
		true,
		CHANNEL_CONNECTIONS);
}

void ServerGameplay::broadcastPacketToAllClients(Packet packet,
	const char *data, size_t size, bool reliable, int channel)
{
	if (!server)
	{
		return;
	}

	for (size_t i = 0; i < server->peerCount; ++i)
	{
		ENetPeer &peer = server->peers[i];
		if (peer.state != ENET_PEER_STATE_CONNECTED)
		{
			continue;
		}

		sendPacket(&peer, packet, data, size, reliable, channel);
	}
}

void ServerGameplay::broadcastPacketToOtherClients(ENetPeer *sourcePeer, Packet packet,
	const char *data, size_t size, bool reliable, int channel)
{
	if (!server)
	{
		return;
	}

	for (size_t i = 0; i < server->peerCount; ++i)
	{
		ENetPeer &peer = server->peers[i];
		if (&peer == sourcePeer || peer.state != ENET_PEER_STATE_CONNECTED)
		{
			continue;
		}

		sendPacket(&peer, packet, data, size, reliable, channel);
	}
}
