#pragma once

#include <glm/vec3.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

struct GameMapDefinition
{
	const char *displayName = "";
	const char *modelFile = "";
	float importScale = 1.0f;
	glm::vec3 position = {};
	glm::vec3 rotationDegrees = {};
};

inline const std::vector<GameMapDefinition> &getGameMaps()
{
	static const std::vector<GameMapDefinition> maps =
	{
		{
			"Among Us",
			RESOURCES_PATH "amongusMap.glb",
			0.1f,
			{0.0f, 0.0f, 0.0f},
			{-90.0f, 0.0f, 0.0f}
		},

				{
			"Circus",
			RESOURCES_PATH "circus.glb",
			4.5f,
			{0.0f, 0.0f, 0.0f},
			{0.0f, 0.0f, 0.0f}
		},


	};

	return maps;
}

inline std::uint32_t getGameMapCount()
{
	return static_cast<std::uint32_t>(getGameMaps().size());
}

inline std::uint32_t clampGameMapIndex(std::uint32_t mapIndex)
{
	const std::uint32_t mapCount = getGameMapCount();
	if (mapCount == 0)
	{
		return 0;
	}

	return (std::min)(mapIndex, mapCount - 1);
}

inline const GameMapDefinition &getGameMapDefinition(std::uint32_t mapIndex)
{
	const auto &maps = getGameMaps();
	return maps[clampGameMapIndex(mapIndex)];
}
