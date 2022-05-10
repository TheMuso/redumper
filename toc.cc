#include <format>
#include <map>
#include <set>
#include <windows.h>
#include <ntddcdrm.h>
#include "cd.hh"
#include "common.hh"
#include "crc16_gsm.hh"
#include "endian.hh"
#include "toc.hh"



namespace gpsxre
{

const char TOC::_ISRC_TABLE[] =
{
	'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '_', '_', '_', '_', '_', '_', 
	'_', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
	'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '_', '_', '_', '_', '_', 
	'_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_'
};


TOC::TOC(const std::vector<uint8_t> &toc_buffer, bool full_toc)
{
	memset(&_q_empty, 0, sizeof(_q_empty));

	if(full_toc)
		InitFullTOC(toc_buffer);
	else
		InitTOC(toc_buffer);
}


TOC::TOC(const ChannelQ *subq, uint32_t sectors_count, int32_t lba_start)
{
	bool track_active = false;
	for(uint32_t lba_index = 0; lba_index < sectors_count; ++lba_index)
	{
		int32_t lba = lba_start + lba_index;

		auto &Q = subq[lba_index];

		if(!memcmp(&Q, &_q_empty, sizeof(_q_empty)) || !Q.Valid())
			continue;

		uint8_t adr = Q.control_adr & 0x0F;
		if(adr == 1)
		{
			if(Q.mode1.tno)
			{
				uint8_t tno = Q.mode1.tno == 0xAA ? Q.mode1.tno : bcd_decode(Q.mode1.tno);

				// new session
				if(sessions.empty() || !sessions.back().tracks.empty() && sessions.back().tracks.back().track_number == 0xAA && tno != 0xAA)
				{
					uint8_t session_number = sessions.empty() ? 1 : sessions.back().session_number + 1;
					sessions.emplace_back().session_number = session_number;
				}

				Session &s = sessions.back();

				// new track
				if(s.tracks.empty() || s.tracks.back().track_number != tno && (s.tracks.back().track_number + 1 == tno || tno == 0xAA))
				{
					if(!s.tracks.empty())
						s.tracks.back().lba_end = lba;

					auto &t = s.tracks.emplace_back();

					t.track_number = tno;
					t.control = Q.control_adr >> 4;
					t.lba_start = lba;
					t.lba_end = lba;
					t.write_offset = 0;
					t.data_mode = 0;
					t.cdi = false;
				}

				Session::Track &t = s.tracks.back();

				uint8_t index = bcd_decode(Q.mode1.index);
				if(index == t.indices.size() + 1)
					t.indices.push_back(lba);

				track_active = true;
			}
			// lead-in
			else
				track_active = false;
		}
		// MCN & ISRC
		else if(adr == 2 || adr == 3)
		{
			;
		}
		// CD-R / CD-RW
		else if(adr == 5)
		{
			track_active = false;
		}
		else
		{
			track_active = false;
		}

		if(track_active)
			sessions.back().tracks.back().lba_end = lba + 1;
	}

	// set session boundaries and remove lead-out entries
	for(auto &s : sessions)
	{
		// if pre-gap is missing a few first sectors (LG/ASUS for instance)
		// extend it to the default size, it will be handled later during split
		if(s.tracks.front().lba_start > lba_start)
		{
			uint32_t lba_index = s.tracks.front().lba_start - lba_start - 1;

			auto &Q = subq[lba_index];
			if(!memcmp(&Q, &_q_empty, sizeof(_q_empty)))
				s.tracks.front().lba_start = s.tracks.front().indices.front() + MSF_LBA_SHIFT;
		}

		if(s.tracks.back().track_number == 0xAA)
			s.tracks.pop_back();
	}
}


void TOC::DeriveINDEX(const TOC &toc)
{
	for(auto &s : sessions)
	{
		for(auto &t : s.tracks)
		{
			for(auto const &toc_s : toc.sessions)
				for(auto const &toc_t : toc_s.tracks)
					if(t.track_number == toc_t.track_number)
					{
						t.indices = toc_t.indices;
						break;
					}
		}
	}
}


void TOC::DeriveControl(const TOC &toc)
{
	for(auto &s : sessions)
	{
		for(auto &t : s.tracks)
		{
			for(auto const &toc_s : toc.sessions)
				for(auto const &toc_t : toc_s.tracks)
					if(t.track_number == toc_t.track_number)
					{
						t.control = toc_t.control;
						break;
					}
		}
	}
}


void TOC::UpdateQ(const ChannelQ *subq, uint32_t sectors_count, int32_t lba_start)
{
	// update session pre-gaps
	for(uint32_t i = 0; i < sessions.size(); ++i)
	{
		Session &s = sessions[i];
		Session::Track &t = s.tracks.front();

		for(int32_t lba = i ? sessions[i - 1].tracks.back().lba_end : lba_start; lba <= t.indices.front(); ++lba)
		{
			uint32_t lba_index = lba - lba_start;
			if(lba_index >= sectors_count)
				break;

			auto &Q = subq[lba_index];

			if(!memcmp(&Q, &_q_empty, sizeof(_q_empty)) || !Q.Valid())
				continue;

			uint8_t adr = Q.control_adr & 0x0F;
			if(adr == 1)
			{
				uint8_t tno = bcd_decode(Q.mode1.tno);
				if(tno == t.track_number)
				{
					t.lba_start = lba;
					break;
				}
			}
		}

		// if pre-gap is missing a few first sectors (LG/ASUS for instance)
		// extend it to the default size, it will be handled later during split
		if(s.tracks.front().lba_start > lba_start)
		{
			uint32_t lba_index = s.tracks.front().lba_start - lba_start - 1;

			auto &Q = subq[lba_index];
			if(!memcmp(&Q, &_q_empty, sizeof(_q_empty)))
				s.tracks.front().lba_start = s.tracks.front().indices.front() + MSF_LBA_SHIFT;
		}
	}

	for(auto &s : sessions)
	{
		for(int32_t i = -1; i <= MSF_LBA_SHIFT; --i)
		{
			int32_t lba = s.tracks.front().indices.front() + i;
		}

		for(uint32_t i = 0, n = (uint32_t)(s.tracks.size() - 1); i < n; ++i)
		{
			int32_t lba = s.tracks[i].indices.front();
			for(; lba < s.tracks[i + 1].indices.front(); ++lba)
			{
				uint32_t lba_index = lba - lba_start;
				if(lba_index >= sectors_count)
					break;

				auto &Q = subq[lba_index];

				if(!memcmp(&Q, &_q_empty, sizeof(_q_empty)) || !Q.Valid())
					continue;

				uint8_t adr = Q.control_adr & 0x0F;
				if(adr == 1)
				{
					uint8_t tno = bcd_decode(Q.mode1.tno);
					if(tno == s.tracks[i + 1].track_number)
						break;
				}
			}

			s.tracks[i].lba_end = lba;
			s.tracks[i + 1].lba_start = lba;
		}
	}

	UpdateINDEX(subq, sectors_count, lba_start);
}


void TOC::UpdateMCN(const ChannelQ *subq, uint32_t sectors_count)
{
	// build linear track array
	std::vector<Session::Track *> tracks;
	for(auto &s : sessions)
		for(auto &t : s.tracks)
			tracks.push_back(&t);
	int32_t track_index = -1;

	for(uint32_t lba_index = 0; lba_index < sectors_count; ++lba_index)
	{
		auto &Q = subq[lba_index];

		if(!memcmp(&Q, &_q_empty, sizeof(_q_empty)) || !Q.Valid())
			continue;

		uint8_t adr = Q.control_adr & 0x0F;
		switch(adr)
		{
		case 1:
			// tracks
			if(Q.mode1.tno != 0 && Q.mode1.tno == 0xAA)
			{
				uint8_t tno = bcd_decode(Q.mode1.tno);
				if(tno < CD_TRACKS_COUNT && track_index + 1 < (int32_t)tracks.size() && tracks[track_index + 1]->track_number == tno)
					++track_index;
			}
			break;

			// MCN
		case 2:
			if(mcn.empty())
			{
				for(uint32_t i = 0; i < sizeof(Q.mode2.mcn); ++i)
					mcn += std::format("{:02}", bcd_decode(Q.mode2.mcn[i]));
				mcn.pop_back();
			}
			break;

			// ISRC
		case 3:
			if(track_index >= 0)
			{
				if(tracks[track_index]->isrc.empty())
				{
					// letters, 5 by 6 bits each
					for(uint32_t i = 0; i < 5; ++i)
					{
						uint8_t c = 0;
						bit_copy(&c, 2, Q.mode3.isrc, i * 6, 6);
						tracks[track_index]->isrc += _ISRC_TABLE[c];
					}

					// padding 2 bits

					// 7 BCD 4-bit numbers
					for(uint32_t i = 4; i < 8; ++i)
						tracks[track_index]->isrc += std::format("{:02}", bcd_decode(Q.mode3.isrc[i]));
					tracks[track_index]->isrc.pop_back();
				}
			}
			break;

			// CD-R / CD-RW
		case 5:
			break;

		default:
			;
		}
	}
}


bool TOC::UpdateCDTEXT(const std::vector<uint8_t> &cdtext_buffer)
{
	bool success = true;

	CDROM_TOC_CD_TEXT_DATA &cd_text_data = *(CDROM_TOC_CD_TEXT_DATA *)cdtext_buffer.data();
	auto descriptors_count = (uint16_t)((cdtext_buffer.size() - sizeof(CDROM_TOC_CD_TEXT_DATA)) / sizeof(cd_text_data.Descriptors[0]));

	// load first available block size information structure and block number statistics
	BlockSizeInfo block_size_info;
	bool bsi_found = false;
	std::set<uint8_t> blocks;
	for(uint16_t i = 0; i < descriptors_count; ++i)
	{
		auto &pack_data = cd_text_data.Descriptors[i];

		//DEBUG
//		std::cout << std::format("{:02X} {:02} {:b} {:02} {:02} {:01} {:b} {}{}{}{}{}{}{}{}{}{}{}{}",
//								 pack_data.PackType, pack_data.TrackNumber, pack_data.ExtensionFlag, pack_data.SequenceNumber, pack_data.CharacterPosition, pack_data.BlockNumber, pack_data.Unicode,
//								 (char)pack_data.Text[0], (char)pack_data.Text[1], (char)pack_data.Text[2], (char)pack_data.Text[3], (char)pack_data.Text[4], (char)pack_data.Text[5],
//								 (char)pack_data.Text[6], (char)pack_data.Text[7], (char)pack_data.Text[8], (char)pack_data.Text[9], (char)pack_data.Text[10], (char)pack_data.Text[11]) << std::endl;

		auto crc = crc16_gsm((uint8_t *)&pack_data, sizeof(pack_data) - sizeof(uint16_t));
		// PLEXTOR PX-W5224TA: crc of last pack is always zeroed
		if(crc != endian_swap(*(uint16_t *)pack_data.CRC) && i + 1 != descriptors_count)
		{
			success = false;
			break;
		}

		if(pack_data.ExtensionFlag)
			continue;

		if((PackType)pack_data.PackType == PackType::SIZE_INFO)
		{
			if(!bsi_found)
			{
				memcpy((char *)&block_size_info + pack_data.TrackNumber * sizeof(pack_data.Text), pack_data.Text, sizeof(pack_data.Text));
				if(pack_data.TrackNumber == 2)
					bsi_found = true;
			}
		}
		else
			blocks.insert(pack_data.BlockNumber);
	}
	
	if(!success)
		return success;

	// dynamically renumerate blocks and store only what's available
	std::vector<uint8_t> blocks_map(8);
	for(auto const &b : blocks)
	{
		blocks_map[b] = (uint8_t)cd_text_lang.size();
		cd_text_lang.push_back(bsi_found ? block_size_info.language_code[b] : 0xFF - sizeof(block_size_info.language_code) + b);
	}

	uint8_t first_track, tracks_count = 0;
	if(bsi_found)
	{
		first_track = block_size_info.first_track;
		tracks_count = block_size_info.last_track - block_size_info.first_track + 1;
	}
	// PLEXTOR PX-W5224TA: no block size info for multisession cd-text
	else
	{
		first_track = sessions.front().tracks.front().track_number;
		for(auto const &s : sessions)
			tracks_count += (uint8_t)s.tracks.size();
	}

	cd_text.resize(cd_text_lang.size());
	for(auto &s : sessions)
		for(auto &t : s.tracks)
			t.cd_text.resize(cd_text_lang.size());

	for(uint16_t i = 0; i < descriptors_count;)
	{
		auto &pack_data = cd_text_data.Descriptors[i];

		if(pack_data.ExtensionFlag)
			continue;

		auto pack_type = (PackType)pack_data.PackType;
		if(IsTextPack(pack_type))
		{
			std::vector<char> text;
			for(; i < descriptors_count && (PackType)cd_text_data.Descriptors[i].PackType == pack_type; ++i)
				text.insert(text.end(), cd_text_data.Descriptors[i].Text, cd_text_data.Descriptors[i].Text + sizeof(cd_text_data.Descriptors[i].Text));

			std::vector<char *> track_texts;

			if(pack_data.Unicode)
			{
				uint16_t *text_unicode = (uint16_t *)text.data();
				uint32_t unicode_size = (uint32_t)text.size() / sizeof(uint16_t);
				for(uint32_t j = 0; j < unicode_size; ++j)
				{
					if(!j || text_unicode[j - 1] == 0)
					{
						auto t = (char *)&text_unicode[j];
						if(!strcmp(t, "\t\t") && !track_texts.empty())
							t = track_texts.back();
						track_texts.push_back(t);
					}
				}
			}
			else
			{
				for(uint32_t j = 0; j < text.size(); ++j)
				{
					if(!j || text[j - 1] == '\0')
					{
						auto t = &text[j];
						if(!strcmp(t, "\t") && !track_texts.empty())
							t = track_texts.back();
						track_texts.push_back(t);
					}
				}
			}

			uint32_t count = std::min((uint32_t)tracks_count + 1, (uint32_t)track_texts.size());
			for(uint32_t j = 0; j < count; ++j)
			{
				CDText *cdt = GetCDText(blocks_map[pack_data.BlockNumber], j ? first_track + j - 1 : 0);

				if(!cdt)
					continue;

				switch(pack_type)
				{
				case PackType::TITLE:
					cdt->title = track_texts[j];
					break;

				case PackType::PERFORMER:
					cdt->performer = track_texts[j];
					break;

				case PackType::SONGWRITER:
					cdt->songwriter = track_texts[j];
					break;

				case PackType::COMPOSER:
					cdt->composer = track_texts[j];
					break;

				case PackType::ARRANGER:
					cdt->arranger = track_texts[j];
					break;

				case PackType::MESSAGE:
					cdt->message = track_texts[j];
					break;

				case PackType::CLOSED_INFO:
					cdt->closed_info = track_texts[j];
					break;

				case PackType::MCN_ISRC:
					cdt->mcn_isrc = track_texts[j];
					break;

				default:
					;
				}
			}
		}
		else
		{
			switch(pack_type)
			{
			case PackType::DISC_ID:
				;
				break;

			case PackType::GENRE_ID:
				;
				break;

			case PackType::TOC:
				;
				break;

			case PackType::TOC2:
				;
				break;

			case PackType::RESERVED1:
			case PackType::RESERVED2:
			case PackType::RESERVED3:
				;
				break;

			default:
				;
			}

			++i;
		}
	}
	
	return success;
}


std::ostream &TOC::Print(std::ostream &os, std::string indent, uint32_t indent_level) const
{
	std::string track_format = std::format("{{:0{}}}", (uint32_t)log10(sessions.back().tracks.back().track_number) + 1);

	bool multisession = sessions.size() > 1;

	for(auto const &s : sessions)
	{
		if(multisession)
			print_multiple(os, indent, indent_level) << std::format("session {}", s.session_number) << std::endl;

		for(auto const &t : s.tracks)
		{
			std::string track_properties;
			int32_t track_length = t.lba_end - t.lba_start;
			if(track_length > 0)
			{
				MSF msf_start = LBA_to_MSF(t.lba_start);
				MSF msf_end = LBA_to_MSF(t.lba_end - 1);
				track_properties = std::format(", LBA: {:6} .. {:6}, length: {:6}, MSF: {:02}:{:02}:{:02}-{:02}:{:02}:{:02} }}",
							   t.lba_start, t.lba_end - 1, track_length,
							   msf_start.m, msf_start.s, msf_start.f, msf_end.m, msf_end.s, msf_end.f);
			}
			print_multiple(os, indent, indent_level + (multisession ? 1 : 0)) << std::format("track {} {{ {}{} }}",
						   std::format(track_format, t.track_number), (t.control & (uint8_t)ChannelQ::Control::DATA) ? " data" : "audio", track_properties) << std::endl;

			auto indices = t.indices;
			indices.insert(indices.begin(), t.lba_start);
			indices.push_back(t.lba_end);

			for(uint32_t i = 0; i < (uint32_t)(indices.size() - 1); ++i)
			{
				int32_t index_start = indices[i];
				int32_t index_end = indices[i + 1];

				int32_t index_length = index_end - index_start;

				// skip index 0
				if(!i && index_length <= 0)
					continue;

				std::string index_properties;

				MSF msf_start = LBA_to_MSF(index_start);
				if(index_length > 0)
				{
					MSF msf_end = LBA_to_MSF(t.lba_end - 1);
					index_properties = std::format("LBA: {:6} .. {:6}, length: {:6}, MSF: {:02}:{:02}:{:02}-{:02}:{:02}:{:02}",
												   index_start, index_end - 1, index_length,
												   msf_start.m, msf_start.s, msf_start.f, msf_end.m, msf_end.s, msf_end.f);
				}
				else
					index_properties = std::format("LBA: {:6}, MSF: {:02}:{:02}:{:02}", index_start, msf_start.m, msf_start.s, msf_start.f);

				print_multiple(os, indent, indent_level + (multisession ? 2 : 1)) << std::format("index {:02} {{ {} }}", i, index_properties) << std::endl;
			}
		}
	}

	return os;
}


std::ostream &TOC::PrintCUE(std::ostream &os, const std::string &image_name, uint32_t cd_text_index) const
{
	std::string track_format = std::format(" (Track {{:0{}}})", (uint32_t)log10(sessions.back().tracks.back().track_number) + 1);

	bool multisession = sessions.size() > 1;

	std::string mcn_print(mcn);
	if(mcn_print.empty() && cd_text_index < cd_text.size() && !cd_text[cd_text_index].mcn_isrc.empty())
		mcn_print = "0" + cd_text[cd_text_index].mcn_isrc;
	if(!mcn_print.empty())
		os << std::format("CATALOG {}", mcn_print) << std::endl;
	if(cd_text_index < cd_text.size())
		PrintCDTextCUE(os, cd_text[cd_text_index], "  ", 0);

	for(uint32_t j = 0; j < sessions.size(); ++j)
	{
		auto &s = sessions[j];

		if(multisession)
		{
			// this is bullshit and impossible to calculate without extracting the whole range between sessions
			// but redump.org requires these values to exist in a CUE-sheet
			constexpr int32_t leadout_size = 6750;
			constexpr int32_t leadin_size = 4500;
			constexpr int32_t pregap_size = 150;

			MSF msf;
			if(j)
			{
				msf = LBA_to_MSF(leadout_size + MSF_LBA_SHIFT);
				os << std::format("REM LEAD-OUT {:02}:{:02}:{:02}", msf.m, msf.s, msf.f) << std::endl;
			}
			os << std::format("REM SESSION {:02}", s.session_number) << std::endl;
			if(j)
			{
				msf = LBA_to_MSF(leadin_size + MSF_LBA_SHIFT);
				os << std::format("REM LEAD-IN {:02}:{:02}:{:02}", msf.m, msf.s, msf.f) << std::endl;
				msf = LBA_to_MSF(pregap_size + MSF_LBA_SHIFT);
				os << std::format("REM PREGAP {:02}:{:02}:{:02}", msf.m, msf.s, msf.f) << std::endl;
			}
		}

		for(auto const &t : s.tracks)
		{
			os << std::format("FILE \"{}{}.bin\" BINARY", image_name, sessions.size() == 1 && sessions.front().tracks.size() == 1 ? "" : std::format(track_format, t.track_number), t.track_number) << std::endl;

			std::string track_type;
			if(t.control & (uint8_t)ChannelQ::Control::DATA)
			{
				std::string track_mode;
				if(t.cdi)
					track_mode = "CDI";
				else
					track_mode = std::format("MODE{}", t.data_mode);
				track_type = std::format("{}/2352", track_mode);
			}
			else
				track_type = "AUDIO";

			os << std::format("  TRACK {:02} {}", t.track_number, track_type) << std::endl;
			if(cd_text_index < t.cd_text.size())
				PrintCDTextCUE(os, t.cd_text[cd_text_index], "  ", 2);

			std::string isrc_print(t.isrc);
			if(isrc_print.empty() && cd_text_index < t.cd_text.size() && !t.cd_text[cd_text_index].mcn_isrc.empty())
				isrc_print = t.cd_text[cd_text_index].mcn_isrc;
			if(!isrc_print.empty())
				os << std::format("    ISRC {}", isrc_print) << std::endl;

			std::string flags;
			if(t.control & (uint8_t)ChannelQ::Control::FOUR_CHANNEL)
				flags += " 4CH";
			if(t.control & (uint8_t)ChannelQ::Control::DIGITAL_COPY)
				flags += " DCP";
			if(t.control & (uint8_t)ChannelQ::Control::PRE_EMPHASIS)
				flags += " PRE";
			if(!flags.empty())
				os << std::format("    FLAGS{}", flags) << std::endl;

			if(!t.indices.empty())
			{
				for(uint32_t i = 0; i <= t.indices.size(); ++i)
				{
					if(!i && t.indices[i] == t.lba_start)
						continue;
					MSF msf = LBA_to_MSF((i == 0 ? 0 : t.indices[i - 1] - t.lba_start) + MSF_LBA_SHIFT);
					os << std::format("    INDEX {:02} {:02}:{:02}:{:02}", i, msf.m, msf.s, msf.f) << std::endl;
				}
			}
		}
	}

	return os;
}


void TOC::InitTOC(const std::vector<uint8_t> &toc_buffer)
{
	// don't rely on anything and sort by session number / track number
	std::map<uint8_t, Session::Track> tracks;

	CDROM_TOC &toc = *(CDROM_TOC *)toc_buffer.data();

	uint16_t descriptors_count = (endian_swap(*(uint16_t *)toc.Length) - 2) / sizeof(toc.TrackData[0]);
	for(uint16_t i = 0; i < descriptors_count; ++i)
	{
		auto &d = toc.TrackData[i];

		if(d.TrackNumber < MAXIMUM_NUMBER_TRACKS || d.TrackNumber == 0xAA)
		{
			int32_t lba = endian_swap(*(int32_t *)d.Address);

			auto &t = tracks[d.TrackNumber];

			t.track_number = d.TrackNumber;
			t.indices.push_back(lba);
			t.control = d.Control;
			t.lba_start = lba;
			t.lba_end = lba;
			t.write_offset = 0;
			t.data_mode = 0;
			t.cdi = false;
		}
	}

	Session &s = sessions.emplace_back();
	s.session_number = 1;
	s.tracks.reserve(tracks.size());
	for(auto const &t : tracks)
		s.tracks.push_back(t.second);

	// set session boundaries and remove lead-out entries
	for(auto &s : sessions)
	{
//		s.tracks.front().lba_start = s.tracks.front().indices.front() + MSF_LBA_SHIFT;

		if(s.tracks.back().track_number == 0xAA)
		{
			int32_t lba_end = s.tracks.back().indices.front();
			s.tracks.pop_back();
			if(!s.tracks.empty())
				s.tracks.back().lba_end = lba_end;
		}
	}
}


void TOC::InitFullTOC(const std::vector<uint8_t> &toc_buffer)
{
	// don't rely on anything and sort by session number / track number
	std::map<uint8_t, std::map<uint8_t, Session::Track>> tracks;

	CDROM_TOC_FULL_TOC_DATA &full_toc_data = *(CDROM_TOC_FULL_TOC_DATA *)toc_buffer.data();
	auto descriptors_count = (uint16_t)((toc_buffer.size() - sizeof(CDROM_TOC_FULL_TOC_DATA)) / sizeof(full_toc_data.Descriptors[0]));
	for(uint16_t i = 0; i < descriptors_count; ++i)
	{
		auto &d = full_toc_data.Descriptors[i];

		// according to the MMC specs, only Q mode 1 and mode 5 are provided here
		if(d.Adr == 1)
		{
			switch(d.Point)
			{
				// first track and last track definitions
			case 0xA0:
			case 0xA1:
				break;

			default:
				if(d.Point < MAXIMUM_NUMBER_TRACKS || d.Point == 0xA2)
				{
					int32_t lba = MSF_to_LBA(*(MSF *)d.Msf);

					auto &t = tracks[d.SessionNumber][d.Point];

					t.indices.push_back(lba);
					t.control = d.Control;
					t.lba_start = lba;
					t.lba_end = lba;
					t.write_offset = 0;
					t.data_mode = 0;
					t.cdi = false;
				}
			}
		}
		// CD-R / CD-RW
		else if(d.Adr == 5)
		{
			;
		}
	}

	for(auto const &t : tracks)
	{
		Session s;
		s.session_number = t.first;
		s.tracks.reserve(t.second.size());
		for(auto const &tt : t.second)
		{
			Session::Track t = tt.second;
			t.track_number = tt.first;
			s.tracks.push_back(t);
		}

		sessions.push_back(s);
	}

	// set session boundaries and remove lead-out entries
	for(auto &s : sessions)
	{
//		s.tracks.front().lba_start = s.tracks.front().indices.front() + MSF_LBA_SHIFT;

		if(s.tracks.back().track_number == 0xA2)
		{
			int32_t lba_end = s.tracks.back().indices.front();
			s.tracks.pop_back();
			if(!s.tracks.empty())
				s.tracks.back().lba_end = lba_end;
		}
	}
}


void TOC::UpdateINDEX(const ChannelQ *subq, uint32_t sectors_count, int32_t lba_start)
{
	for(auto &s : sessions)
	{
		for(auto &t : s.tracks)
		{
			for(int32_t lba = t.lba_start; lba < t.lba_end; ++lba)
			{
				uint32_t lba_index = lba - lba_start;
				if(lba_index >= sectors_count)
					break;

				auto &Q = subq[lba_index];

				if(!memcmp(&Q, &_q_empty, sizeof(_q_empty)) || !Q.Valid())
					continue;

				uint8_t adr = Q.control_adr & 0x0F;
				if(adr == 1)
				{
					uint8_t tno = bcd_decode(Q.mode1.tno);
					uint8_t index = bcd_decode(Q.mode1.index);
					if(tno == t.track_number && index == t.indices.size() + 1)
						t.indices.push_back(lba);
				}
			}
		}
	}
}


std::ostream &TOC::PrintCDTextCUE(std::ostream &os, const CDText &cdt, std::string indent, uint32_t indent_level)
{
	if(!cdt.title.empty())
		print_multiple(os, indent, indent_level) << std::format("TITLE \"{}\"", cdt.title) << std::endl;
	if(!cdt.performer.empty())
		print_multiple(os, indent, indent_level) << std::format("PERFORMER \"{}\"", cdt.performer) << std::endl;
	if(!cdt.songwriter.empty())
		print_multiple(os, indent, indent_level) << std::format("SONGWRITER \"{}\"", cdt.songwriter) << std::endl;
//	if(!cdt.composer.empty())
//		print_multiple(os, indent, indent_level) << std::format("REM COMPOSER \"{}\"", cdt.composer) << std::endl;
//	if(!cdt.arranger.empty())
//		print_multiple(os, indent, indent_level) << std::format("REM ARRANGER \"{}\"", cdt.arranger) << std::endl;

	return os;
}


bool TOC::IsTextPack(PackType pack_type)
{
	return pack_type == PackType::TITLE || pack_type == PackType::PERFORMER || pack_type == PackType::SONGWRITER || pack_type == PackType::COMPOSER || pack_type == PackType::ARRANGER ||
		pack_type == PackType::MESSAGE || pack_type == PackType::CLOSED_INFO || pack_type == PackType::MCN_ISRC;
}


TOC::CDText *TOC::GetCDText(uint8_t index, uint8_t track_number)
{
	CDText *cdt = nullptr;

	if(track_number)
	{
		for(auto &s : sessions)
			for(auto &t : s.tracks)
				if(t.track_number == track_number)
				{
					cdt = &t.cd_text[index];
					break;
				}
	}
	else
		cdt = &cd_text[index];

	return cdt;
}

}
