/* fqc: what this program does
*
* Copyright (C) 2019 Guilherme De Sena Brandine
*
* Authors: Guilherme De Sena Brandine
*
* This program is free software: you can redistribute it and/or
* modify it under the terms of the GNU General Public License as
* published by the Free Software Foundation, either version 3 of the
* License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful, but
* WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* General Public License for more details.
*/
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>

#include <iostream>
#include <iomanip>
#include <fstream>

#include <vector>
#include <ctime>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <utility>

#include "smithlab_utils.hpp"
#include "smithlab_os.hpp"
#include "OptionParser.hpp"

using std::string;
using std::runtime_error;
using std::cerr;
using std::endl;
using std::vector;
using std::reverse;
using std::ostream;
using std::ofstream;
using std::cout;
using std::unordered_map;
using std::pair;
using std::make_pair;

// converts 64 bit integer to a sequence string by reading 3 bits at a time and
// converting back to ACTGN
static string
int_to_seq(size_t v, const size_t &kmer_size) {
  string ans;
  for (size_t i = 0; i < kmer_size; ++i) {
    switch (v & 7) {
      case 0: ans.push_back('A'); break;
      case 1: ans.push_back('C'); break;
      case 2: ans.push_back('T'); break;
      case 3: ans.push_back('G'); break;
      case 7: ans.push_back('N'); break;
    }
    v >>= 3;
  }
  if (v > 0)
    throw std::runtime_error("bad kmer");

  reverse(ans.begin(), ans.end());
  return ans;
}

int main(int argc, const char **argv) {
  clock_t begin = clock();
  /****************** COMMAND LINE OPTIONS ********************/
  string filename;  // fastq file
  string outfile;
  size_t kmer_size = 8;
  bool VERBOSE = false;

  OptionParser opt_parse(strip_path(argv[0]),
                         "Quality control metrics for fastq files",
                         "<fastq-file>");

  // Length of k-mers to count. Grows memory exponentially so it is bound to 10
  opt_parse.add_opt("kmer", 'k',
                    "k-mer size (default = 8)", false, kmer_size);

  opt_parse.add_opt("outfile", 'o',
                    "filename to save results (default = stdout)",
                    false, outfile);

  opt_parse.add_opt("verbose", 'v', "print more run info", false, VERBOSE);
  vector<string> leftover_args;

  opt_parse.parse(argc, argv, leftover_args);
  if (argc == 1 || opt_parse.help_requested()) {
    cerr << opt_parse.help_message() << endl
    << opt_parse.about_message() << endl;
    return EXIT_SUCCESS;
  }
  if (opt_parse.about_requested()) {
    cerr << opt_parse.about_message() << endl;
    return EXIT_SUCCESS;
  }
  if (opt_parse.option_missing()) {
    cerr << opt_parse.option_missing_message() << endl;
    return EXIT_SUCCESS;
  }
  if (leftover_args.size() != 1) {
    cerr << opt_parse.help_message() << endl;
    return EXIT_SUCCESS;
  }

  if (kmer_size > 10) {
    cerr << "K-mer size should not exceed 10\n";
    cerr << opt_parse.help_message() << endl;
    return EXIT_SUCCESS;
  }

  filename = leftover_args.front();
  /****************** END COMMAND LINE OPTIONS *****************/


  /****************** MEMORY MAP ******************************/
  // open the file
  int fd = open(filename.c_str(), O_RDONLY, 0);
  if (fd == -1)
    throw runtime_error("failed to open fastq file: " + filename);

  // get the file size
  struct stat st;
  fstat(fd, &st);

  // execute mmap
  void* mmap_data = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (mmap_data == MAP_FAILED)
    throw runtime_error("failed to mmap fastq file: " + filename);

  cerr << "Fastq size: " << st.st_size /(1 << 20)<< "Mb" << endl;
  char *first = static_cast<char*>(mmap_data);
  char *last = first + st.st_size - 1;

  /*************** DEFINE QC METRICS ************************/

  // This can be a very large number, the maximum illumina read size. Does not
  // affect memory very much
  const size_t kNumBases = 1000;
  const double ascii_to_quality = 33.0;
  const size_t num_chars = 8;  // A = 000, C = 001, T = 010, G = 011, N = 111

  /***********ALL****************/
  vector<size_t> num_bases_per_pos(num_chars, 0);  // counts for each base
  vector<size_t> quality_per_pos(num_chars, 0);  // quality per position in read
  size_t total_bases = 0;
  size_t avg_read_length;
  double gc_content;
  double n_content;
  double seq_duplication_level;
  size_t min_read_length = 0;
  size_t max_read_length = 0;
  size_t exp_kmer_obs;  // Num reads * (length - k + 1) / 4^k assuming iid

  /*********** PER BASE METRICS ****************/

  // counts the number of bases in every read position
  vector<vector<size_t>> base_count(num_chars, vector<size_t>(kNumBases, 0));

  // Sum of base qualities in every read position
  vector<vector<size_t>> base_quality(num_chars, vector<size_t>(kNumBases, 0));

  /*********** PER READ METRICS ***************/
  unordered_map <size_t, size_t> read_freq;

  // Distribution of read lengths
  vector<size_t> read_length_freq(kNumBases, 0);

  /********** KMER FREQUENCY ****************/

  const size_t kmer_mask = (1ll << (3*kmer_size)) - 1;

  // A 3^(K+1) vector to count all possible kmers
  vector<size_t> kmer_count(kmer_mask + 1, 0);

  /********** PASS THROUGH FILE *************/
  size_t base_ind = 0;  // 0,1,2,3 or 7
  size_t read_pos = 0;  // which base we are at in the read
  size_t nreads = 0;  // total number of reads in fastq
  char c;  // to avoid accessing *curr multiple times

  // Temporarily store the second read of the 4 lines to know the base to which
  // quality characters are associated
  vector<int16_t> buff(kNumBases, 0);

  size_t cur_kmer = 0;  // kmer hash
  size_t read_rk_hash = 0;  // Rabin-Karp read hash

  if (VERBOSE) {
    cerr << "Started analysis of " << filename << "\n";
    cerr << "K-mer size: " << kmer_size << "\n";
  }


  // read character by character
  for (char *curr = first; curr < last;) {
    ++nreads;

    // *************READ NAME LINE****************************/
    // fast forward first line
    for (; *curr != '\n'; ++curr) {}

    ++curr;  // skips \n from line 1

    // *************NUCLEOTIDE LINE***************************/
    read_pos = 0;
    for (; *curr != '\n'; ++curr) {
      c = *curr;

      // Transforms base into 3-bit index
      // Bits 2,3 and 4 of charcters A,C,G,T and N are distinct so we can just
      // use them to get the index instead of doing if-elses.
      base_ind = (c >> 1) & 7;

      // Increments count of base
      base_count[base_ind][read_pos]++;

      // Need this to know what base the quality is associated to, therefore
      // we will store each read in memory
      buff[read_pos] = base_ind;
       cur_kmer = ((cur_kmer << 3) | base_ind);

      // If we already read >= k bases, increment the k-mer count
      if (read_pos >= kmer_size - 1)
        kmer_count[cur_kmer & kmer_mask]++;

      read_rk_hash = read_rk_hash*8 + base_ind;
      read_pos++;
    }
    ++curr;  // skips \n from line 2

    // Store read statistics
    read_length_freq[read_pos]++;  // registers the read length
    read_freq[read_rk_hash]++;  // increments rk hash count

    // Reset read statistics
    read_pos = 0;
    read_rk_hash = 0;

    // *************OTHER READ NAME LINE**********************/
    // fast forward third line
    for (; *curr != '\n'; ++curr) {}
    ++curr;  // skips \n from line 3

    // *************QUALITY LINE******************************/
    for (; (*curr != '\n') && (curr < last); ++curr) {
      // Gets the base from buffer, finds index, adds quality value
      base_quality[buff[read_pos]][read_pos] += *curr;
      read_pos++;
    }
    ++curr;  // skip \n or increments from last which is not a problem
  }

  // Deallocates memory
  munmap(mmap_data, st.st_size);

  if (VERBOSE)
    cerr << "Finished reading file.\n";

  /**************** SUMMARIZE *********************************/
  // Calculates summaries based on collected data
  if (VERBOSE)
    cerr << "Calculating statistics...\n";

  // Total frequency of each base and total number of reads
  for (size_t j = 0; j < num_chars; ++j) {
    for (size_t i = 0; i < kNumBases; ++i) {
      num_bases_per_pos[j] += base_count[j][i];
      if (i == 1)
        quality_per_pos[j] += base_quality[j][i];
      total_bases += base_count[j][i];
    }
  }

  for (size_t i = 0; i < kNumBases; ++i) {
    if (read_length_freq[i] > 0) {
      // First nonzero is min read length
      if (min_read_length == 0)
        min_read_length = i;

      // Last nonzero is max read length
      max_read_length = i;
    }
  }

  // Averages
  avg_read_length = total_bases / nreads;
  gc_content = 100.0 * (num_bases_per_pos[1] + num_bases_per_pos[3])
             / static_cast<double>(total_bases);

  n_content = 100.0 * num_bases_per_pos[7] / static_cast<double>(total_bases);

  // Expected number of kmer observations from iid poisson
  exp_kmer_obs = static_cast<size_t>(nreads * (avg_read_length - kmer_size + 1)
               / static_cast<double>( (1ll << (2*kmer_size))));

  /************** WRITE TO OUTPUT *********************************/
  // define output
  ofstream of;
  if (!outfile.empty()) of.open(outfile.c_str(), ofstream::binary);
  ostream os(outfile.empty() ? cout.rdbuf() : of.rdbuf());

  // Basic statistics
  os << "number_of_reads\t" << nreads << "\n";
  os << "number_of_bases\t" << total_bases << "\n";
  os << "average_read_length\t" << avg_read_length << "\n";
  os << "minimum_read_length\t" << min_read_length << "\n";
  os << "maximum_read_length\t" << max_read_length << "\n";
  os << "gc_frequency\t" << gc_content << "\n";
  os << "n_frequency\t" << n_content << "\n";

  // Per base statistics
  vector<char> bases = {'A', 'C', 'G', 'T', 'N'};
  for (auto base : bases) {
    int16_t base_ind = ((base >>1) & 7);
    os << base << "_base_quality\t ";
    for (size_t i = 0; i < avg_read_length; ++i) {
      double qual =  base_quality[base_ind][i]
                   / static_cast<double>(base_count[base_ind][i]);
      qual -= ascii_to_quality;
      if (i < avg_read_length - 1)
        os << qual << ",";
      else
        os << qual << "\n";
    }

    os << base << "_frequency ";
    for (size_t i = 0; i < avg_read_length; ++i) {
      const double denom = (base_count[0][i] + base_count[1][i] +
                            base_count[2][i] + base_count[3][i] +
                            base_count[7][i]);
      if (i < avg_read_length - 1)
        os << base_count[base_ind][i]/denom << ",";
      else
        os << base_count[base_ind][i]/denom << "\n";
    }
  }

  // Sequence duplication
  size_t dup_reads = 0;
  for (auto v : read_freq)
    if (v.second > 1)
      dup_reads += v.second;

  seq_duplication_level = 100.0 * dup_reads / static_cast<double>(nreads);
  os << "seq_duplication_level\t" << seq_duplication_level << "\n";

  // Kmer statistics
  os << "\n";
  os << "kmer_size\t" << kmer_size << "\n";
  os << "kmer_expected_frequency\t " << exp_kmer_obs << "\n";

  // Store frequent kmers
  vector<pair<size_t, size_t>> kmer_freq_ordered;
  for (size_t i = 0; i < kmer_mask; ++i)
    if (kmer_count[i] > 5 * exp_kmer_obs)
      kmer_freq_ordered.push_back(make_pair(i, kmer_count[i]));

  // sort by frequency in decreasing order
  sort(kmer_freq_ordered.begin(), kmer_freq_ordered.end(),
      [](auto &a, auto &b){ return a.second > b.second; });

  os << "Overrepresented k-mers (> 5 stdevs above poisson): \n";
  for (auto v : kmer_freq_ordered)
    os << int_to_seq(v.first, kmer_size) << "\t" << v.second  << "\n";

  /************** TIME SUMMARY *********************************/
  // TODO(gui) : find adapters with significant kmer enrichment
  if (VERBOSE)
    cerr << "Elapsed time: "
         << (clock() - begin) / CLOCKS_PER_SEC
         << " seconds\n";
}

