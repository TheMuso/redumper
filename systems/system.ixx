module;
#include <filesystem>
#include <ostream>
#include <string>

export module systems.system;

import readers.sector_reader;



namespace gpsxre
{

export class System
{
public:
	enum class Type
	{
		RAW_DATA,
		RAW_AUDIO,
		ISO
	};

	virtual ~System() = default;

	virtual std::string getName() = 0;
	virtual Type getType() = 0;
	virtual void printInfo(std::ostream &os, SectorReader *sector_reader, const std::filesystem::path &track_path) const = 0;
};

}
