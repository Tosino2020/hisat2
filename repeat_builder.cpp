/*
 * Copyright 2018, Chanhee Park <parkchanhee@gmail.com> and Daehwan Kim <infphilo@gmail.com>
 *
 * This file is part of HISAT 2.
 *
 * HISAT 2 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HISAT 2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HISAT 2.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <iostream>
#include <vector>
#include <algorithm>
#include "timer.h"

#include "repeat_builder.h"

unsigned int levenshtein_distance(const std::string& s1, const std::string& s2) 
{
	const std::size_t len1 = s1.size(), len2 = s2.size();
	std::vector<unsigned int> col(len2+1), prevCol(len2+1);

	for (unsigned int i = 0; i < prevCol.size(); i++)
		prevCol[i] = i;
	for (unsigned int i = 0; i < len1; i++) {
		col[0] = i+1;
		for (unsigned int j = 0; j < len2; j++)
			// note that std::min({arg1, arg2, arg3}) works only in C++11,
			// for C++98 use std::min(std::min(arg1, arg2), arg3)
			col[j+1] = std::min( std::min(prevCol[1 + j] + 1, col[j] + 1), prevCol[j] + (s1[i]==s2[j] ? 0 : 1) );
		col.swap(prevCol);
	}
	return prevCol[len2];
}

bool checkSequenceMergeable(const string& s1, const string& s2, TIndexOffU max_edit = 10)
{
	unsigned int ed = levenshtein_distance(s1, s2);
	return (ed <= max_edit);
}

typedef pair<TIndexOffU, TIndexOffU> Range;
static const Range EMPTY_RANGE = Range(1, 0);

struct RepeatRange {
	RepeatRange() {
        forward = true;
    };
	RepeatRange(Range r, int id) : 
		range(r), rg_id(id), forward(true) {};
    RepeatRange(Range r, int id, bool fw) :
        range(r), rg_id(id), forward(fw) {};

	Range range;
	int rg_id;
    bool forward;
};

Range reverseRange(const Range& r, TIndexOffU size)
{
	size_t len = r.second - r.first;
	Range rc;

	rc.first = size - r.second;
	rc.second = rc.first + len;

	return rc;
}

string reverseComplement(const string& str)
{
	string rc;
	for(TIndexOffU si = str.length(); si > 0; si--) {
		rc.push_back(asc2dnacomp[str[si - 1]]);
	}
	return rc;
}

/**
 * @brief return true iff a U b = a 
 *
 * @param a
 * @param b
 *
 * @return 
 */
static bool checkRangeMergeable(const Range& a, const Range& b)
{
	if(a.first <= b.first && a.second >= b.second) {
		return true;
	}

	return false;
}


static bool compareRepeatRangeByRange(const RepeatRange& a, const RepeatRange& b)
{
	if((a.range.second > b.range.second) ||
			(a.range.second == b.range.second && a.range.first < b.range.first)) {
		return true;
	}

	return false;
}

static bool compareRepeatRangeByRgID(const RepeatRange& a, const RepeatRange& b)
{
	return a.rg_id < b.rg_id;
}


template<typename index_t>
static bool compareRepeatCoordByJoinedOff(const RepeatCoord<index_t>& a, const RepeatCoord<index_t>& b)
{
	return a.joinedOff < b.joinedOff;
}


template<typename TStr>
string getString(const TStr& ref, TIndexOffU start, size_t len)
{
	string s;
	size_t ref_len = ref.length();

	for(size_t i = 0; (i < len) && (start + i < ref_len); i++) {
		char nt = "ACGT"[ref[start + i]];
		s.push_back(nt);
	}

	return s;
}

template<typename TStr>
void masking_with_N(TStr& s, TIndexOffU start, size_t length)
{
	size_t s_len = s.length();

	for(size_t pos = 0; (pos < length) && (start + pos < s_len); pos++) {
		s[start + pos] = 0x04;
	}
}

template<typename TStr>
void dump_tstr(const TStr& s)
{
	static int print_width = 60;

	size_t s_len = s.length();

	for(size_t i = 0; i < s_len; i += print_width) {
		string buf;
		for(size_t j = 0; (j < print_width) && (i + j < s_len); j++) {
			buf.push_back("ACGTN"[s[i + j]]);
		}
		cerr << buf << endl;
	}
	cerr << endl;
}


template<typename TStr>
NRG<TStr>::NRG(
		EList<RefRecord>& szs,
		EList<string>& ref_names,
		TStr& s,
		string& filename,
		BlockwiseSA<TStr>& sa,
        bool forward = true) :
	szs_(szs), ref_namelines_(ref_names), 
	s_(s), filename_(filename), bsa_(sa), forward_(forward)
{
	cerr << "NRG: " << filename_ << endl;

	// build ref_names_ from ref_namelines_
    buildNames();
    buildJoinedFragment();
}

template<typename TStr>
void NRG<TStr>::build(TIndexOffU rpt_len,
                      TIndexOffU rpt_cnt,
                      bool flagGrouping,
                      TIndexOffU rpt_edit)
{
	TIndexOffU count = 0;

	EList<RepeatCoord<TIndexOffU> > rpt_positions;
	TIndexOffU min_lcp_len = s_.length();

	while(count < s_.length() + 1) {
		TIndexOffU saElt = bsa_.nextSuffix();
		count++;
		
		if(count && (count % 1000000 == 0)) {
			cerr << "SA count " << count << endl;
		}

		if(rpt_positions.size() == 0) {
			rpt_positions.expand();
			rpt_positions.back().joinedOff = saElt;
			rpt_positions.back().fw = forward_;
		} else {
			TIndexOffU prev_saElt = rpt_positions.back().joinedOff;

			// calculate common prefix length between two text.
			//   text1 is started from prev_saElt and text2 is started from saElt
			int lcp_len = getLCP(prev_saElt, saElt);

			if(lcp_len >= rpt_len) {
				rpt_positions.expand();
				rpt_positions.back().joinedOff = saElt;
				rpt_positions.back().fw = forward_;

				if(min_lcp_len > lcp_len) {
					min_lcp_len = lcp_len;
				}

			} else {
				if (rpt_positions.size() >= rpt_cnt) {
                    sort(rpt_positions.begin(), 
                            rpt_positions.begin() + rpt_positions.size(),
                            compareRepeatCoordByJoinedOff<TIndexOffU>);

					string ss = getString(s_, prev_saElt, min_lcp_len);

					addRepeatGroup(ss, rpt_positions);
				}

				// flush previous positions 
				rpt_positions.resize(1);
				rpt_positions.back().joinedOff = saElt;
				rpt_positions.back().fw = forward_;
				min_lcp_len = s_.length();
			}
		}

#if 0//{{{
		// DK - debugging purposes
		if(count < 100) {
			suffixes.expand();
			cerr << setw(12) << saElt << "\t";
			for(int k = 0; k < 100; k++) {
				char nt = "ACGT"[s[saElt+k]];
				cerr << nt;
				suffixes.back().push_back(nt);
			}
			cerr << endl;

			if(count > 1) {
				SwAligner al;
				SwResult res;

				SimpleFunc scoreMin;
				scoreMin.init(SIMPLE_FUNC_LINEAR, 0.0f, -0.2f);

				SimpleFunc nCeil;
				nCeil.init(SIMPLE_FUNC_LINEAR, 0.0f, 0, 2.0f, 0.1f);

				const string& str1 = suffixes[suffixes.size() - 2];
				const string& str2 = suffixes[suffixes.size() - 1];

				string qual = "";
				for(int i = 0; i < str1.length(); i++) {
					qual.push_back('I');
				}

				// Set up penalities
				Scoring sc(
						DEFAULT_MATCH_BONUS,     // constant reward for match
						DEFAULT_MM_PENALTY_TYPE,     // how to penalize mismatches
						30,        // constant if mm pelanty is a constant
						30,        // penalty for decoded SNP
						0,
						0,
						scoreMin,       // min score as function of read len
						nCeil,          // max # Ns as function of read len
						DEFAULT_N_PENALTY_TYPE,      // how to penalize Ns in the read
						DEFAULT_N_PENALTY,          // constant if N pelanty is a constant
						DEFAULT_N_CAT_PAIR,      // true -> concatenate mates before N filtering
						25,  // constant coeff for cost of gap in read
						25,  // constant coeff for cost of gap in ref
						15, // linear coeff for cost of gap in read
						15, // linear coeff for cost of gap in ref
						1,    // # rows at top/bot only entered diagonally
						0,   // canonical splicing penalty
						0,   // non-canonical splicing penalty
						0);  // conflicting splice site penalt

				doTestCase2(
						al,
						str1.c_str(),
						qual.c_str(),
						str2.c_str(),
						0,
						sc,
						DEFAULT_MIN_CONST,
						DEFAULT_MIN_LINEAR,
						res);
			}
		}
#endif//}}}

	}

    mergeRepeatGroup();
    if (flagGrouping) {
        groupRepeatGroup(rpt_edit);
    }
	//adjustRepeatGroup(flagGrouping);
	// we found repeat_group
	cerr << "CP " << rpt_grp_.size() << " groups found" << endl;

	//dump_tstr(s);

	// write to FA
	// sequence
	// repeat sequeuce
	//saveFile();
}

template<typename TStr>
void NRG<TStr>::buildNames()
{
	ref_names_.resize(ref_namelines_.size());
	for(size_t i = 0; i < ref_namelines_.size(); i++) {
		string& nameline = ref_namelines_[i];

		for(size_t j = 0; j < nameline.length(); j++) {
			char n = nameline[j];
			if(n == ' ') {
				break;
			}
			ref_names_[i].push_back(n);
		}
	}
}

template<typename TStr>
int NRG<TStr>::mapJoinedOffToSeq(TIndexOffU joined_pos)
{

	/* search from cached_list */
	if (num_cached_ > 0) {
		for (int i = 0; i < num_cached_; i++) {
			Fragments *frag = &cached_[i];
			if (frag->contain(joined_pos)) {
				return frag->frag_id;
			}
		}
		/* fall through */
	}

	/* search list */
	int top = 0;
	int bot = fraglist_.size() - 1; 
	int pos = 0;

	Fragments *frag = &fraglist_[pos];
	while ((bot - top) > 1) {
		pos = top + ((bot - top) >> 1);
		frag = &fraglist_[pos];

		if (joined_pos < frag->start) {
			bot = pos;
		} else {
			top = pos;
		}
	}

	frag = &fraglist_[top];
	if (frag->contain(joined_pos)) {
		// update cache
		if (num_cached_ < CACHE_SIZE_JOINEDFRG) {
			cached_[num_cached_] = *frag;
			num_cached_++;
		} else {
			cached_[victim_] = *frag;
			victim_++; // round-robin
			victim_ %= CACHE_SIZE_JOINEDFRG;
		}

		return top;
	}

	return -1;
}

template<typename TStr>
int NRG<TStr>::getGenomeCoord(TIndexOffU joined_pos, 
		string& chr_name, TIndexOffU& pos_in_chr)
{
	int seq_id = mapJoinedOffToSeq(joined_pos);
	if (seq_id < 0) {
		return -1;
	}

	Fragments *frag = &fraglist_[seq_id];
	TIndexOffU offset = joined_pos - frag->start;

	pos_in_chr = frag->start_in_seq + offset;
	chr_name = ref_names_[frag->seq_id];

	return 0;
}

template<typename TStr>
void NRG<TStr>::buildJoinedFragment()
{
	int n_seq = 0;
	int n_frag = 0;

	for(size_t i = 0; i < szs_.size(); i++) {
		if(szs_[i].len > 0) n_frag++;
		if(szs_[i].first && szs_[i].len > 0) n_seq++;
	}

	int npos = 0;
	int seq_id = -1;
	TIndexOffU acc_frag_length = 0;
	TIndexOffU acc_ref_length = 0;
	fraglist_.resize(n_frag + 1);

	for(size_t i = 0; i < szs_.size(); i++) {
		if(szs_[i].len == 0) {
			continue;
		}

		fraglist_[npos].start = acc_frag_length;
		fraglist_[npos].length = szs_[i].len;
		fraglist_[npos].start_in_seq = acc_ref_length + szs_[i].off;
		fraglist_[npos].frag_id = i;
		fraglist_[npos].frag_id = npos;
		if(szs_[i].first) {
			seq_id++;
			fraglist_[npos].first = true;
		}
		fraglist_[npos].seq_id = seq_id;

		acc_frag_length += szs_[i].len;
		acc_ref_length += szs_[i].off + szs_[i].len;

		npos++;
	}

	// Add Last Fragment(empty)
	fraglist_[npos].start = acc_frag_length;
	fraglist_[npos].length = 0;
	fraglist_[npos].start_in_seq = acc_ref_length + szs_.back().off;
}

template<typename TStr>
void NRG<TStr>::sortRepeatGroup()
{
	if(rpt_grp_.size() > 0) {
		sort(rpt_grp_.begin(), rpt_grp_.begin() + rpt_grp_.size(), 
                compareRepeatGroupByJoinedOff);
	}
}

template<typename TStr>
void NRG<TStr>::saveRepeatGroup()
{
	string rptinfo_filename = filename_ + ".rep.info";

	int rpt_count = rpt_grp_.size();
	TIndexOffU acc_pos = 0;

	ofstream fp(rptinfo_filename.c_str());

	for(size_t i = 0; i < rpt_count; i++) {
		RepeatGroup& rg = rpt_grp_[i];
		EList<RepeatCoord<TIndexOffU> >& positions = rg.positions;

		// >rpt_name*0\trep\trep_pos\trep_len\tpos_count\t0
		// chr_name:pos:direction chr_name:pos:direction
		//
		// >rep1*0	rep	0	100	470	0
		// 22:112123123:+ 22:1232131113:+
		//

		// Header line
		fp << ">" << "rpt_" << i << "*0";
		fp << "\t" << "rep"; // TODO
		fp << "\t" << acc_pos;
		fp << "\t" << rg.seq.length();
		fp << "\t" << positions.size();
		fp << "\t" << "0"; 
        // debugging
        // fp << "\t" << rg.seq;
		fp << endl; 

		acc_pos += rg.seq.length();

		// Positions
        for(size_t j = 0; j < positions.size(); j++) {
			if(j && (j % 10 == 0)) {
				fp << endl;
			}

			if(j % 10) {
				fp << " ";
			}

			string chr_name;
			TIndexOffU pos_in_chr;

			getGenomeCoord(positions[j].joinedOff, chr_name, pos_in_chr);
			char direction = positions[j].fw ? '+':'-';

			fp << chr_name << ":" << pos_in_chr << ":" << direction;
		}
		fp << endl;
	}		
	fp.close();
}
	
	
template<typename TStr>
void NRG<TStr>::saveRepeatSequence()
{
	string fname = filename_ + ".rep.fa";

	ofstream fp(fname.c_str());

	/* TODO */
	fp << ">" << "rep" << endl;

	int oskip = 0;

	for(TIndexOffU grp_idx = 0; grp_idx < rpt_grp_.size(); grp_idx++) {
		RepeatGroup& rg = rpt_grp_[grp_idx];
        size_t seq_len = rg.seq.length();

		TIndexOffU si = 0;
		while(si < seq_len) {
			size_t out_len = std::min((size_t)(output_width - oskip), (size_t)(seq_len - si));

			fp << rg.seq.substr(si, out_len);

			if((oskip + out_len) == output_width) {
				fp << endl;
				oskip = 0;
			} else {
				// last line
				oskip = oskip + out_len;
			}

			si += out_len;
		}
	}
	if(oskip) {
		fp << endl;
	}

	fp.close();
}

template<typename TStr>
void NRG<TStr>::saveFile()
{
	saveRepeatSequence();
	saveRepeatGroup();
}


template<typename TStr>
void NRG<TStr>::merge(NRG<TStr>& prev_repeat_groups)
{

}



/**
 * TODO
 * @brief 
 *
 * @param rpt_seq
 * @param rpt_range
 */
template<typename TStr>
void NRG<TStr>::addRepeatGroup(const string& rpt_seq, const EList<RepeatCoord<TIndexOffU> >& positions)
{
#if 0
	// rpt_seq is always > 0
	//
	const int rpt_len = rpt_seq.length();

	for (int i = 0; i < rpt_grp_.size(); i++) {
		RepeatGroup& rg = rpt_grp_[i];
		string& rseq = rg.rpt_seq;
		const int rlen = rseq.length();
		if (rlen == 0) {
			// skip
			continue;
		}

		if (rlen > rpt_len) {
			// check if rpt_seq is substring of rpt_groups sequeuce
			if (rseq.find(rpt_seq) != string::npos) {
				// substring. exit
				return;
			}
		} else if (rlen <= rpt_len) {
			// check if rpt_groups sequeuce is substring of rpt_seq
			if (rpt_seq.find(rseq) != string::npos) {
				// remove rseq
				rg.rpt_seq = "";
			}
		}
	}
#endif

	// add to last
	rpt_grp_.expand();
	rpt_grp_.back().seq = rpt_seq;
	rpt_grp_.back().positions = positions;
}


/**
 * @brief 
 *
 * @tparam TStr
 */
template<typename TStr> 
void NRG<TStr>::mergeRepeatGroup()
{
	int range_count = 0;
	for(size_t i = 0; i < rpt_grp_.size(); i++) {
		range_count += rpt_grp_[i].positions.size();
	}

	cerr << "CP " << "range_count " << range_count << endl;

	if(range_count == 0) {
		cerr << "CP " << "no repeat sequeuce" << endl; 
		return;
	}

	EList<RepeatRange> rpt_ranges;
	rpt_ranges.reserveExact(range_count);

	for(size_t i = 0; i < rpt_grp_.size(); i++) {
		RepeatGroup& rg = rpt_grp_[i];
		size_t s_len = rg.seq.length();

		for(size_t j = 0; j < rg.positions.size(); j++) {
			rpt_ranges.push_back(RepeatRange(
                        make_pair(rg.positions[j].joinedOff, rg.positions[j].joinedOff + s_len),
                        i, rg.positions[j].fw));
		}
	}

    assert_gt(rpt_ranges.size(), 0);

	sort(rpt_ranges.begin(), rpt_ranges.begin() + rpt_ranges.size(), 
            compareRepeatRangeByRange);

	// Merge
	int merged_count = 0;
	for(size_t i = 0; i < rpt_ranges.size() - 1;) {
		size_t j = i + 1;
		for(; j < rpt_ranges.size(); j++) {
			// check i, j can be merged 
			//

			if(!checkRangeMergeable(rpt_ranges[i].range, rpt_ranges[j].range)) {
				break;
			}

			rpt_ranges[j].range = EMPTY_RANGE;
			rpt_ranges[j].rg_id = std::numeric_limits<int>::max();
			merged_count++;
		}
		i = j;
	}

	cerr << "CP ";
	cerr << "merged_count: " << merged_count;
	cerr << endl;

#if 1
	{
		string fname = filename_ + ".rptinfo";
		ofstream fp(fname.c_str());

		for(size_t i = 0; i < rpt_ranges.size(); i++) {
			if(rpt_ranges[i].range == EMPTY_RANGE) {
				continue;
			}
			Range rc = reverseRange(rpt_ranges[i].range, s_.length());
			fp << "CP " << i ;
			fp << "\t" << rpt_ranges[i].range.first;
			fp << "\t" << rpt_ranges[i].range.second;
			fp << "\t" << rpt_grp_[rpt_ranges[i].rg_id].seq;
			fp << "\t" << rc.first;
			fp << "\t" << rc.second;
			fp << "\t" << reverseComplement(rpt_grp_[rpt_ranges[i].rg_id].seq);
			fp << "\t" << rpt_ranges[i].rg_id;
			fp << endl;
		}

		fp.close();
	}
#endif


    /* remake RepeatGroup from rpt_ranges */

	// sort by rg_id
	sort(rpt_ranges.begin(), rpt_ranges.begin() + rpt_ranges.size(), 
            compareRepeatRangeByRgID);

	EList<RepeatGroup> mgroup;

	mgroup.reserveExact(rpt_grp_.size());
	mgroup.swap(rpt_grp_);

	for(size_t i = 0; i < rpt_ranges.size() - 1;) {
		if(rpt_ranges[i].rg_id == std::numeric_limits<int>::max()) {
			break;
		}

		size_t j = i + 1;
		for(; j < rpt_ranges.size(); j++) {
			if(rpt_ranges[i].rg_id != rpt_ranges[j].rg_id) {
				break;
			}
		}

		/* [i, j) has a same rg_id */

		int rg_id = rpt_ranges[i].rg_id;
		rpt_grp_.expand();
		rpt_grp_.back().seq = mgroup[rg_id].seq;
		for (int k = i; k < j; k++) {
			rpt_grp_.back().positions.push_back(
                    RepeatCoord<TIndexOffU>(0, 0,
                        rpt_ranges[k].range.first,
                        rpt_ranges[k].forward));
		}

		// sort positions
        assert_gt(rpt_grp_.back().positions.size(), 0);
        sort(rpt_grp_.back().positions.begin(), 
                rpt_grp_.back().positions.begin()
                + rpt_grp_.back().positions.size(),
                compareRepeatCoordByJoinedOff<TIndexOffU>);

		i = j;
	}

}

template<typename TStr>
void NRG<TStr>::groupRepeatGroup(TIndexOffU rpt_edit)
{
    if (rpt_grp_.size() == 0) {
        cerr << "CP " << "no repeat group" << endl;
        return;
    }

    cerr << "CP " << "before grouping " << rpt_grp_.size() << endl;

    int step = rpt_grp_.size() >> 8;

    if(step == 0) {step = 1;}
    cerr << "CP " << step << endl;

    Timer timer(cerr, "Total time for grouping sequences: ", true);

    for(size_t i = 0; i < rpt_grp_.size() - 1; i++) {
        if(i % step == 0) {
            cerr << "CP " << i << "/" << rpt_grp_.size() << endl;
        }

        if(rpt_grp_[i].empty()) {
            // empty -> skip
            continue;
        }

        string& str1 = rpt_grp_[i].seq;

        for(size_t j = i + 1; j < rpt_grp_.size(); j++) {
            string& str2 = rpt_grp_[j].seq;

            if(checkSequenceMergeable(str1, str2, rpt_edit)) {
                /* i, j merge into i */
                rpt_grp_[i].merge(rpt_grp_[j]);

                rpt_grp_[j].set_empty();
            }
        }
    }

	EList<RepeatGroup> mgroup;
    mgroup.reserveExact(rpt_grp_.size());
    mgroup.swap(rpt_grp_);

    for(size_t i = 0; i < mgroup.size(); i++) {
        if (!mgroup[i].empty()) {
            rpt_grp_.expand();
            rpt_grp_.back() = mgroup[i];
        }
    }

    cerr << "CP " << "after merge " << rpt_grp_.size() << endl;

#if 1
    {
        string fname = filename_ + ".altseq";
        ofstream fp(fname.c_str());

        for (int i = 0; i < rpt_grp_.size(); i++) {
            RepeatGroup& rg = rpt_grp_[i];
            if (rg.empty()) {
                continue;
            }
            fp << "CP " << i ;
            fp << "\t" << rg.seq;
            for (int j = 0; j < rg.alt_seq.size(); j++) {
                fp << "\t" << rg.alt_seq[j];
            }
            fp << endl;
        }

        fp.close();
    }
#endif
}


#if 0
/**
 * @brief Remove empty repeat group
 */
template<typename TStr>
void NRG<TStr>::adjustRepeatGroup(bool flagGrouping, TIndexOffU rpt_edit)
{
	cerr << "CP " << "repeat_group size " << rpt_grp_.size() << endl;

	int range_count = 0;
	for(size_t i = 0; i < rpt_grp_.size(); i++) {
		range_count += rpt_grp_[i].positions.size();
	}

	cerr << "CP " << "range_count " << range_count << endl;

	if(range_count == 0) {
		cerr << "CP " << "no repeat sequeuce" << endl; 
		return;
	}

	cerr << "Build RepeatRange" << endl;

	EList<RepeatRange> rpt_ranges;
	rpt_ranges.reserveExact(range_count);

	for(size_t i = 0; i < rpt_grp_.size(); i++) {
		RepeatGroup& rg = rpt_grp_[i];
		size_t s_len = rg.seq.length();

		for(size_t j = 0; j < rg.positions.size(); j++) {
			rpt_ranges.push_back(RepeatRange(
                        make_pair(rg.positions[j].joinedOff, rg.positions[j].joinedOff + s_len),
                        i, rg.positions[j].fw));
		}
	}
    assert_eq(rpt_ranges.size(), range_count);

	sort(rpt_ranges.begin(), rpt_ranges.begin() + rpt_ranges.size(), 
            compareRepeatRangeByRange);

	// Dump
#if 0
	for (int i = 0; i < rpt_ranges.size(); i++) {
		cerr << "CP " << i ;
		cerr << "\t" << rpt_ranges[i].range.first;
		cerr << "\t" << rpt_ranges[i].range.second;
		cerr << "\t" << rpt_grp_[rpt_ranges[i].rg_id].rpt_seq;
		cerr << "\t" << rpt_ranges[i].rg_id;
		cerr << endl;
	}
#endif

#if 1	
	// Merge
    assert_gt(rpt_ranges.size(), 0);
	int merged_count = 0;
	for(size_t i = 0; i < rpt_ranges.size() - 1;) {
		size_t j = i + 1;
		for(; j < rpt_ranges.size(); j++) {
			// check i, j can be merged 
			//

			if(!checkRangeMergeable(rpt_ranges[i].range, rpt_ranges[j].range)) {
				break;
			}

			rpt_ranges[j].range = EMPTY_RANGE;
			rpt_ranges[j].rg_id = std::numeric_limits<int>::max();
			merged_count++;
		}
		i = j;
	}

	cerr << "CP ";
	cerr << "merged_count: " << merged_count;
	cerr << endl;
#endif

	// sort by rg_id
	sort(rpt_ranges.begin(), rpt_ranges.begin() + rpt_ranges.size(), 
            compareRepeatRangeByRgID);

	// Dump
#if 1
	{
		string fname = filename_ + ".rptinfo";
		ofstream fp(fname.c_str());

		for(size_t i = 0; i < rpt_ranges.size(); i++) {
			if(rpt_ranges[i].range == EMPTY_RANGE) {
				continue;
			}
			Range rc = reverseRange(rpt_ranges[i].range, s_.length());
			fp << "CP " << i ;
			fp << "\t" << rpt_ranges[i].range.first;
			fp << "\t" << rpt_ranges[i].range.second;
			fp << "\t" << rpt_grp_[rpt_ranges[i].rg_id].seq;
			fp << "\t" << rc.first;
			fp << "\t" << rc.second;
			fp << "\t" << reverseComplement(rpt_grp_[rpt_ranges[i].rg_id].seq);
			fp << "\t" << rpt_ranges[i].rg_id;
			fp << endl;
		}

		fp.close();
	}
#endif

	/***********/

	/* rebuild rpt_grp_ */
	EList<RepeatGroup> mgroup;

	mgroup.reserveExact(rpt_grp_.size());
	mgroup.swap(rpt_grp_);

	for(size_t i = 0; i < rpt_ranges.size() - 1;) {
		if(rpt_ranges[i].rg_id == std::numeric_limits<int>::max()) {
			break;
		}

		size_t j = i + 1;
		for(; j < rpt_ranges.size(); j++) {
			if(rpt_ranges[i].rg_id != rpt_ranges[j].rg_id) {
				break;
			}
		}

		/* [i, j) has a same rg_id */

		int rg_id = rpt_ranges[i].rg_id;
		rpt_grp_.expand();
		rpt_grp_.back().seq = mgroup[rg_id].seq;
		for (int k = i; k < j; k++) {
			rpt_grp_.back().positions.push_back(rpt_ranges[k].range.first);
		}

		// sort positions
		rpt_grp_.back().positions.sort();

		i = j;
	}


	if (flagGrouping) {
		cerr << "CP " << "before grouping " << rpt_grp_.size() << endl;
		int step = rpt_grp_.size() >> 8;

		if (step == 0) {step = 1;}
		cerr << "CP " << step << endl;
		Timer timer(cerr, "Total time for grouping sequences: ", true);

		for (int i = 0; i < rpt_grp_.size() - 1; i++) {

			if (i % step == 0) {
				cerr << "CP " << i << "/" << rpt_grp_.size() << endl;
			}

			if (rpt_grp_[i].empty()) {
				// empty -> skip
				continue;
			}

			string& str1 = rpt_grp_[i].seq;


			for (int j = i + 1; j < rpt_grp_.size(); j++) {
				string& str2 = rpt_grp_[j].seq;

				if (checkSequenceMergeable(str1, str2, rpt_edit)) {
					/* i, j merge into i */
					rpt_grp_[i].merge(rpt_grp_[j]);

					rpt_grp_[j].set_empty();
				}
			}
		}

		mgroup.clear();
		mgroup.reserveExact(rpt_grp_.size());
		mgroup.swap(rpt_grp_);

		for (int i = 0; i < mgroup.size(); i++) {
			if (!mgroup[i].empty()) {
				rpt_grp_.expand();
				rpt_grp_.back() = mgroup[i];
			}
		}

		cerr << "CP " << "after merge " << rpt_grp_.size() << endl;
#if 1
		{
			string fname = filename_ + ".altseq";
			ofstream fp(fname.c_str());

			for (int i = 0; i < rpt_grp_.size(); i++) {
				RepeatGroup& rg = rpt_grp_[i];
				if (rg.empty()) {
					continue;
				}
				fp << "CP " << i ;
				fp << "\t" << rg.seq;
				for (int j = 0; j < rg.alt_seq.size(); j++) {
					fp << "\t" << rg.alt_seq[j];
				}
				fp << endl;
			}

			fp.close();
		}
#endif
	}


}
#endif



template<typename TStr>
void NRG<TStr>::repeat_masking()
{
	for(size_t i = 0; i < rpt_grp_.size(); i++) {
		RepeatGroup *rg = &rpt_grp_[i];

		size_t rpt_sqn_len = rg->seq.length();

		for(size_t j = 0; j < rg->positions.size(); j++) {
			TIndexOffU pos = rg->positions[j].joinedOff;

			// masking [pos, pos + rpt_sqn_len) to 'N'
			masking_with_N(s_, pos, rpt_sqn_len);
		}
	}
}

// Dump
//
// to_string
static string to_string(int val)
{
	stringstream ss;
	ss << val;
	return ss.str();
}


template<typename TStr>
int get_lcp_back(TStr& s, TIndexOffU a, TIndexOffU b)
{
	int k = 0;
	TIndexOffU s_len = s.length();

	if (a == s_len || b == s_len) {
		return 0;
	}

	while ((a - k) > 0 && (b - k) > 0) {
		if (s[a - k - 1] != s[b - k - 1]) {
			break;
		}
		k++;
	}

	return k;
}

#if 0
int get_lcp(string a, string b)
{
    int k = 0;
    int a_len = a.length();
    int b_len = b.length();
    
    while (k < a_len && k < b_len) {
        if (a[k] != b[k]) {
            break;
        }
        k++;
    }
    
    return k;
}
#endif

template<typename TStr>
int NRG<TStr>::getLCP(TIndexOffU a, TIndexOffU b)
{
    int k = 0;
    TIndexOffU s_len = s_.length();
    
    if (a >= s_len || b >= s_len) {
        return 0;
    }
    
#if 1
    size_t a_end = 0;
    size_t b_end = 0;

    if (forward_) {
        int a_frag_id = mapJoinedOffToSeq(a);
        int b_frag_id = mapJoinedOffToSeq(b);

        if (a_frag_id < 0 || b_frag_id < 0) {
            cerr << "CP " << a_frag_id << ", " << b_frag_id << endl;
            return 0;
        }

        a_end = fraglist_[a_frag_id].start + fraglist_[a_frag_id].length;
        b_end = fraglist_[b_frag_id].start + fraglist_[b_frag_id].length;
    } else {
        // ReverseComplement
        // a, b are positions w.r.t reverse complement string.
        // fragment map is based on forward string
        int a_frag_id = mapJoinedOffToSeq(s_len - a - 1);
        int b_frag_id = mapJoinedOffToSeq(s_len - b - 1);

        if (a_frag_id < 0 || b_frag_id < 0) {
            cerr << "CP " << a_frag_id << ", " << b_frag_id << endl;
            return 0;
        }

        a_end = s_len - fraglist_[a_frag_id].start;
        b_end = s_len - fraglist_[b_frag_id].start;
    }
    
    assert_leq(a_end, s_len);
    assert_leq(b_end, s_len);
#else
    size_t a_end = s_len;
    size_t b_end = s_len;
#endif
    
    
    while ((a + k) < a_end && (b + k) < b_end) {
        if (s_[a + k] != s_[b + k]) {
            break;
        }
        k++;
    }
    
    return k;
}


/****************************/
template class NRG<SString<char> >;
template void dump_tstr(const SString<char>& );
template static bool compareRepeatCoordByJoinedOff(
        const RepeatCoord<TIndexOffU>& , const RepeatCoord<TIndexOffU>&);
