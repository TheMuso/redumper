#pragma once



#include <cstdint>
#include "cd.hh"



namespace gpsxre
{

class Scrambler
{
public:
	Scrambler();
	bool IsScrambled(uint8_t *sector) const;
	bool Unscramble(uint8_t *sector_unscrambled, const uint8_t *sector, int32_t lba) const;
	void Process(uint8_t *sector_unscrambled, const uint8_t *sector) const;

private:
	uint8_t _table[CD_DATA_SIZE];

	void GenerateTable();
};

}
