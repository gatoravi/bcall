/*  bcall.cc -- main

    Copyright (c) 2016, Avinash Ramu

    Author: Avinash Ramu <avinash3003@yahoo.co.in>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.  */

#include <bitset>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include "cereal/types/unordered_map.hpp"
#include "cereal/types/memory.hpp"
#include "cereal/archives/binary.hpp"
#include "gzstream/gzstream.h"
#include "Rmath.h"

using namespace std;

//Map from chromosome to integer
std::map<string, int> chr_to_int = {
    {"1", 0}, {"2", 1}, {"3", 2}, {"4", 3},
    {"5", 4}, {"6", 5}, {"7", 6}, {"8", 7},
    {"9", 8}, {"10", 9}, {"11", 10}, {"12", 11},
    {"13", 12}, {"14", 13}, {"15", 14}, {"16", 15},
    {"17", 16}, {"18", 17}, {"19", 18}, {"20", 19},
    {"21", 20}, {"22", 21}, {"X", 22}, {"Y", 23},
    {"MT", 24}
};

//key is sampleID, value is path to readcount.gz file
std::unordered_map<string, string> sample_to_readcountfile;

//struct to store accumulated read counts
struct readcounts {
    uint64_t total_ref_count;
    uint64_t total_alt_count;
    template <class Archive>
    void serialize(Archive& ar) {
        ar(total_ref_count, total_alt_count);
    }
};

//key is pos << 5 | chr_int, value is struct readcounts
std::unordered_map<uint64_t, readcounts> site_readcounts;

//usage message
int usage() {
    cerr << endl << "./bcall ";
    cerr << endl << "\t prior-and-call file_with_mpileupcounts op_variants_file_name";
    cerr << endl << "\t prior-dump file_with_mpileupcounts op_priors_dump_file_name";
    cerr << endl << "\t prior-dump-fixed file_with_mpileupcounts op_priors_dump_file_name fixed-sites.bed.gz";
    cerr << endl << "\t prior-merge priors_dump_file_list";
    cerr << endl;
    cerr << endl << "The input file has two columns, sample_name and path to "
                    "\nfile with readcounts that have been compressed with "
                    "\nbgzip/gzip, for e.g `SRR1 SRR1_readcounts.gz`";
    cerr << endl << "The prior-dump command creates a file that is a C++ map serialized using cereal "
                    "\nand written to disk. This can then be read by a different "
                    "\nprocess. The prior-dump-fixed only looks at sites specified by the bed.gz file.";
    cerr << endl << "The prior-merge command requires a file that has two columns, "
                    "\ndump_name and path to dump file.";
    cerr << endl << endl;
    return 0;
}

//Create a key that is of type double
//The key is unique for a chr:pos combination
//Left shift the position, AND the chr bits
uint64_t create_key(string chr, uint32_t pos) {
    int chr_int = chr_to_int[chr];
    /* //see binary encoding
    bitset<5> x(chr_int);
    cerr << "chr_x is " << x << endl;
    bitset<32> pos_x(pos);
    cerr << "pos_x is " << pos_x << endl;
    cerr << "unique key is " << unique_key << endl;
    bitset<64> key_x(unique_key);
    cerr << "key_x is " << key_x << endl;
    */
    uint64_t unique_key = (pos << 5) | chr_int;
    return unique_key;
}

//Print output header
void print_header(ostream& out = cout) {
    out << "sample" << "\t"
        << "p_value" << "\t"
        << "chr" << "\t"
        << "pos" << "\t"
        << "depth" << "\t"
        << "ref_base" << "\t"
        << "refcount" << "\t"
        << "altcount" << "\t"
        << "acount" << "\t"
        << "ccount" << "\t"
        << "gcount" << "\t"
        << "tcount" << "\t"
        << "ncount" << "\t"
        << "indelcount"
        << endl;
}

//Print output header
inline void print_out_line(string sample, double p_value, string line, ostream& out = cout) {
    out << sample << "\t" << p_value << "\t" << line << endl;
}

//Takes a line of the readcount file as input and applies
//the binomial test, the `p` is calculated using all the
//readcounts at this site across samples
void apply_model_readcount_line(string sample, string line, bool fixed_sites = false) {
    stringstream ss(line);
    string chr, ref;
    uint32_t pos, depth, ref_count, alt_count;
    ss >> chr >> pos >> depth >> ref;
    ss >> ref_count >> alt_count;
    if(chr_to_int.find(chr) != chr_to_int.end()) {
        uint64_t key = create_key(chr, pos);
        if(site_readcounts.find(key) == site_readcounts.end()) {
            throw runtime_error("Unable to find chr/pos " + chr + " " + to_string(pos));
        }
        double total_rc = site_readcounts[key].total_ref_count + site_readcounts[key].total_alt_count;
        double prior_p =
            (double)site_readcounts[key].total_ref_count / (double) total_rc;
        //(1 - pbinom(8, 10, 0.5)) * 2  == binom.test(9, 10, 0.5, alternative="t")
        double p_value = (1 - pbinom(alt_count, ref_count + alt_count - 1, prior_p, true, false)) * 2;
        if (p_value < 0.05 && ref_count != 0 && alt_count != 0) {
            print_out_line(sample, p_value, line);
        }
    }
}

//parse a line from the readcount file
void parse_readcount_line(string sample, string line, bool fixed_sites = false) {
    stringstream ss(line);
    string chr, ref;
    uint32_t pos, depth, ref_count, alt_count;
    ss >> chr >> pos >> depth >> ref;
    ss >> ref_count >> alt_count;
    if(chr_to_int.find(chr) != chr_to_int.end()) {
        uint64_t key = create_key(chr, pos);
        if(site_readcounts.find(key) == site_readcounts.end()) {
            //Sites are fixed by the BED file, don't add new sites
            if (fixed_sites) {
                return;
            }
            site_readcounts[key].total_ref_count = 0;
            site_readcounts[key].total_alt_count = 0;
        }
        site_readcounts[key].total_ref_count += ref_count;
        site_readcounts[key].total_alt_count += alt_count;
    }
}

//iterate through readcount file - And apply model to each line
//first arg is name of the gz file
//second argument is the function to apply to each line of the file
void parse_readcount_file(string sample, string gzfile, function<void(string, string, bool)> func,
                          bool fixed_sites = false) {
    igzstream in(gzfile.c_str());
    cerr << "Opening " << gzfile << endl;
    std::string line;
    int line_count = 0;
    std::getline(in, line); //Skip header
    while(std::getline(in, line)){
        func(sample, line, fixed_sites);
        line_count += 1;
    }
    if(!line_count) {
        throw runtime_error("Readcount file empty - " + gzfile);
    }
    cerr << "Read " << line_count << " lines from " << gzfile << endl;
}

//Iterate through each sample's readcounts
void calculate_priors(bool fixed_sites = false) {
    function<void(string, string, bool)> parse_line = parse_readcount_line;
    for (auto& kv : sample_to_readcountfile) {
        cerr << "Processing " << kv.first << endl;
        parse_readcount_file(kv.first, kv.second, parse_line, fixed_sites);
        cerr << "Size of readcount map is " << site_readcounts.size() << endl;
    }
}

//Iterate through each sample's readcounts and call
void apply_model() {
    function<void(string, string, bool)> apply_model_line = apply_model_readcount_line;
    for (auto& kv : sample_to_readcountfile) {
        cerr << "Applying model to " << kv.first << endl;
        parse_readcount_file(kv.first, kv.second, apply_model_line);
    }
}

//Print the priors for each site
void print_priors(bool print_zeros = true) {
    for (auto& kv : site_readcounts) {
        if(print_zeros == false &&
           kv.second.total_ref_count == 0 &&
           kv.second.total_alt_count == 0) {
            continue;
        }
        cerr << "site " << kv.first;
        cerr << " ref_c " << kv.second.total_ref_count;
        cerr << " alt_c " << kv.second.total_alt_count;
        cerr << endl;
    }
}

//Write the priors map to a file
void write_priors(const string& output_file) {
    ofstream fout(output_file);
    if (!fout.is_open()) {
        throw runtime_error("unable to open " + output_file +
                            " for writing.");
    }
    cereal::BinaryOutputArchive archive(fout);
    archive(site_readcounts);
    fout.close();
}

//Read the priors map from a file
void read_priors() {
    for (auto& kv : sample_to_readcountfile) {
        cout << "Reading dump " << kv.first << endl;
        string prior_file = kv.second;
        ifstream fin(prior_file);
        if (!fin.is_open()) {
            throw runtime_error("unable to open " + prior_file +
                                " for reading priors.");
        }
        cereal::BinaryInputArchive archive(fin);
        std::unordered_map<uint64_t, readcounts> temp_site_readcounts;
        archive(temp_site_readcounts);
        //Aggregate temp with main
        for (auto& kv : temp_site_readcounts) {
            if (site_readcounts.find(kv.first) == site_readcounts.end()) {
                site_readcounts[kv.first] = kv.second;
            } else {
                site_readcounts[kv.first].total_ref_count += kv.second.total_ref_count;
                site_readcounts[kv.first].total_alt_count += kv.second.total_alt_count;
            }
        }
        fin.close();
    }
}

//Iterate through the samples file and calc site-specific priors
void read_samples(char* samples_file) {
    ifstream sample_fh(samples_file, ios::in);
    string line;
    string sample, readcountfile;
    int line_count = 0;
    while (getline(sample_fh, line)) {
        stringstream iss(line);
        iss >> sample >> readcountfile;
        sample_to_readcountfile[sample] = readcountfile;
        line_count++;
    }
    if(!line_count) {
        throw runtime_error("Sample file empty - " + string(samples_file));
    }
}

void add_bedline_to_map(string line) {
    string chr;
    uint32_t start, end;
    stringstream ss(line);
    ss >> chr >> start >> end;
    for (uint32_t pos = start + 1; pos <= end; pos++) {
        uint64_t key = create_key(chr, pos);
        //Initialize total_ref and total_alt to zero
        site_readcounts[key] = {0, 0};
    }
}

//Read a BED file and only store those sites in the map
void initialize_fixed_map(string bedFile) {
    igzstream in(bedFile.c_str());
    cerr << "Initializing map with sites in  " << bedFile << endl;
    std::string line;
    int line_count = 0;
    std::getline(in, line); //Skip header
    while(std::getline(in, line)){
        add_bedline_to_map(line);
        line_count += 1;
    }
    if(!line_count) {
        throw runtime_error("Bedfile empty - " + bedFile);
    }
    cerr << "Read " << line_count << " lines from " << bedFile << endl;
    cerr << "Size of readcount map is " << site_readcounts.size() << endl;
}

int main(int argc, char* argv[]) {
    try {
        if(argc >= 4) {
            read_samples(argv[2]);
            if (string(argv[1]) == "prior-and-call") {
                    calculate_priors();
                    //print_priors();
                    print_header();
                    apply_model();
                    return 0;
            }
            else if (string(argv[1]) == "prior-dump") {
                    calculate_priors();
                    print_priors();
                    write_priors(string(argv[3]));
                    return 0;
            }
            else if (argc > 4 && string(argv[1]) == "prior-dump-fixed") {
                    initialize_fixed_map(string(argv[4]));
                    calculate_priors(true);
                    print_priors(false);
                    write_priors(string(argv[3]));
                    return 0;
            }
        }
        if (argc == 3 && string(argv[1]) == "prior-merge") {
            read_samples(argv[2]);
            read_priors();
            print_priors();
            return 0;
        }
    } catch (const runtime_error& e) {
        cerr << e.what() << endl;
        return 1;
    }
    return usage();
}
