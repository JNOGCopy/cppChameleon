#pragma once

#include <packet.h>

#include <cstdint>
#include <glm/vec2.hpp>
#include <map>
#include <string>
#include <vector>

struct ClientNetworking
{
	enum class RoundResult
	{
		None,
		HunterWon,
	};

	enum class RoundPhase
	{
		Lobby,
		HiderHide,
		HunterSearch,
	};

	struct RemotePlayerState
	{
		glm::vec3 position = {};
		float yaw = 0.0f;
		std::int32_t animationIndex = 0;
	};

	struct RemotePaintTextureUpdate
	{
		int meshIndex = -1;
		glm::ivec2 size = {};
		int quality = 0;
		std::vector<unsigned char> pixels;
	};

	enum class ConnectionState
	{
		Disconnected,
		Connecting,
		Connected,
		Failed,
	};

	bool connectToServer(const char *serverAddress);
	bool sendPlayerState(const Packet_PlayerStateUpdate &playerState, bool reliable);
	bool sendHunterHitPlayer(std::uint64_t targetCID);
	bool sendPaintTextureUpdate(int meshIndex, glm::ivec2 size, int quality,
		const std::vector<unsigned char> &pixels);
	void update(float deltaTime);
	void shutdown();

	void receiveDataFromServer(ENetEvent &event);

	const char *getConnectionStateName() const;
	bool isHunter(std::uint64_t cid) const;
	bool isLocalHunter() const;
	bool isPlayerFound(std::uint64_t cid) const;

	ENetHost *client = nullptr;
	ENetPeer *serverPeer = nullptr;
	ConnectionState connectionState = ConnectionState::Disconnected;
	std::string connectedServerAddress = "localhost";
	std::string lastStatus = "Disconnected";
	std::uint64_t localCID = 0;
	bool receivedPlayerData = false;
	bool gameActive = false;
	std::uint64_t hunterCID = 0;
	RoundPhase roundPhase = RoundPhase::Lobby;
	float roundTimerRemainingSeconds = 0.0f;
	bool localPlayerFound = false;
	RoundResult roundResult = RoundResult::None;
	std::map<std::uint64_t, RemotePlayerState> remotePlayers;
	std::map<std::uint64_t, std::map<int, RemotePaintTextureUpdate>> remotePaintUpdates;
	std::vector<std::uint64_t> foundHiderIDs;
};
