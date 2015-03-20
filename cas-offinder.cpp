#include "config.h"

#include "cas-offinder.h"
#include "oclfunctions.h"

#include <sstream>
#include <iterator>

using namespace std;

void Cas_OFFinder::set_complementary_sequence(cl_char* seq, size_t seqlen) {
	size_t i, l = 0;
	cl_char tmp;

	for (i = 0; i < seqlen; i++) {
		if (seq[i] == 'A') seq[i] = 'T';
		else if (seq[i] == 'T') seq[i] = 'A';
		else if (seq[i] == 'G') seq[i] = 'C';
		else if (seq[i] == 'C') seq[i] = 'G';
		else if (seq[i] == 'R') seq[i] = 'Y';
		else if (seq[i] == 'Y') seq[i] = 'R';
		else if (seq[i] == 'M') seq[i] = 'K';
		else if (seq[i] == 'K') seq[i] = 'M';
		else if (seq[i] == 'H') seq[i] = 'D';
		else if (seq[i] == 'D') seq[i] = 'H';
		else if (seq[i] == 'B') seq[i] = 'V';
		else if (seq[i] == 'V') seq[i] = 'B';
	}
	for (i = 0; i < seqlen / 2; i++) {
		tmp = seq[i];
		seq[i] = seq[seqlen - i - 1];
		seq[seqlen - i - 1] = tmp;
	}
}

void Cas_OFFinder::set_seq_flags(int* seq_flags, const cl_char* seq, size_t seqlen) {
	int i, n = 0;
	for (i = 0; i < seqlen; i++) {
		if (seq[i] != 'N') {
			seq_flags[n] = i;
			n++;
		}
	}
	if (i != n)
		seq_flags[n] = -1;
}

void Cas_OFFinder::initOpenCL(cl_device_type devtype) {
	unsigned int i;

	cl_device_id devices[MAX_DEVICE_NUM];
	cl_uint device_cnt;

	m_devnum = 0;
	for (i = 0; i < platform_cnt; i++) {
		oclGetDeviceIDs(platforms[i], devtype, MAX_DEVICE_NUM - m_devnum, devices + m_devnum, &device_cnt);
		m_devnum += device_cnt;
	}

	if (m_devnum == 0) {
		cout << "No OpenCL devices found." << endl;
		exit(1);
	}

	cl_context context;
	cl_program program;

	const size_t src_len = strlen(program_src);
	for (i = 0; i < m_devnum; i++) {
		// Create completely separate contexts per device to avoid unknown errors
		context = oclCreateContext(0, 1, &devices[i], 0, 0);
		m_contexts.push_back(context);
		program = oclCreateProgramWithSource(context, 1, &program_src, &src_len);
		oclBuildProgram(program, 1, &devices[i], "", 0, 0);
		m_finderkernels.push_back(oclCreateKernel(program, "finder"));
		m_comparerkernels.push_back(oclCreateKernel(program, "comparer"));
		m_queues.push_back(oclCreateCommandQueue(m_contexts[i], devices[i], 0));
		MAX_ALLOC_MEMORY.push_back(0);
		oclGetDeviceInfo(devices[i], CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof(cl_ulong), &MAX_ALLOC_MEMORY[i], 0);
	}
	cout << "Total " << m_devnum << " device(s) found." << endl;
}

Cas_OFFinder::Cas_OFFinder(cl_device_type devtype) {
	initOpenCL(devtype);
}

Cas_OFFinder::~Cas_OFFinder() {
	unsigned int i;
	for (i = 0; i < m_finderkernels.size(); i++)
		oclReleaseKernel(m_finderkernels[i]);
	for (i = 0; i < m_comparerkernels.size(); i++)
		oclReleaseKernel(m_comparerkernels[i]);
	for (i = 0; i < m_devnum; i++) {
		oclReleaseCommandQueue(m_queues[i]);
		oclReleaseContext(m_contexts[i]);
	}
	clearbufvec(&m_patternbufs);
	clearbufvec(&m_patternflagbufs);
	clearbufvec(&m_comparebufs);
	clearbufvec(&m_compareflagbufs);
	clearbufvec(&m_entrycountbufs);
}

void Cas_OFFinder::setChrData() {
	unsigned int dev_index;

	m_chrdatasize = chrdata.size();
	m_totalanalyzedsize = 0;
	m_lasttotalanalyzedsize = 0;
	m_lastloci = 0;

	m_dicesizes.clear();
	clearbufvec(&m_chrdatabufs);
	clearbufvec(&m_flagbufs);
	clearbufvec(&m_locibufs);

	for (dev_index = 0; dev_index < m_devnum; dev_index++) {
		m_dicesizes.push_back(
			(size_t)MIN(
			(MAX_ALLOC_MEMORY[dev_index] - sizeof(cl_char)* (3 * m_patternlen - 1) - sizeof(cl_uint)* (2 * m_patternlen + 3) - sizeof(cl_ushort)) / (4 * sizeof(cl_char) + 3 * sizeof(cl_uint) + 2 * sizeof(cl_ushort)),
			((m_chrdatasize / m_devnum) + ((m_chrdatasize%m_devnum == 0) ? 0 : 1))
			)
			); // No more than maximum allocation per device
		// cout << "Dicesize: " << m_dicesizes[dev_index] << endl;
		m_chrdatabufs.push_back(oclCreateBuffer(m_contexts[dev_index], CL_MEM_READ_ONLY, sizeof(cl_char)* (m_dicesizes[dev_index] + m_patternlen - 1), 0));
		m_flagbufs.push_back(oclCreateBuffer(m_contexts[dev_index], CL_MEM_WRITE_ONLY, sizeof(cl_char)* m_dicesizes[dev_index], 0));
		m_locibufs.push_back(oclCreateBuffer(m_contexts[dev_index], CL_MEM_WRITE_ONLY, sizeof(cl_uint)* m_dicesizes[dev_index], 0));

		oclSetKernelArg(m_finderkernels[dev_index], 0, sizeof(cl_mem), &m_chrdatabufs[dev_index]);
		oclSetKernelArg(m_finderkernels[dev_index], 4, sizeof(cl_mem), &m_flagbufs[dev_index]);
		oclSetKernelArg(m_finderkernels[dev_index], 6, sizeof(cl_mem), &m_locibufs[dev_index]);

		oclSetKernelArg(m_comparerkernels[dev_index], 0, sizeof(cl_mem), &m_chrdatabufs[dev_index]);
		oclSetKernelArg(m_comparerkernels[dev_index], 1, sizeof(cl_mem), &m_locibufs[dev_index]);
		oclSetKernelArg(m_comparerkernels[dev_index], 7, sizeof(cl_mem), &m_flagbufs[dev_index]);
	}
}

bool Cas_OFFinder::loadNextChunk() {
	if (m_totalanalyzedsize == m_chrdatasize)
		return false;

	unsigned int dev_index;
	unsigned long long tailsize;

	m_activedevnum = 0;
	m_worksizes.clear();
	m_lasttotalanalyzedsize = m_totalanalyzedsize;

	for (dev_index = 0; dev_index < m_devnum; dev_index++) {
		tailsize = m_chrdatasize - m_totalanalyzedsize;
		m_activedevnum++;
		if (tailsize <= m_dicesizes[dev_index]) {
			oclEnqueueWriteBuffer(m_queues[dev_index], m_chrdatabufs[dev_index], CL_TRUE, 0, (size_t)(sizeof(cl_char)* (tailsize + m_patternlen - 1)), (cl_char *)chrdata.c_str() + m_totalanalyzedsize, 0, 0, 0);
			m_totalanalyzedsize += tailsize;
			m_worksizes.push_back(tailsize);
#ifdef DEBUG
			cout << "Worksize: " << m_worksizes[dev_index] << ", Tailsize: " << tailsize << endl;
#endif
			break;
		}
		else {
			oclEnqueueWriteBuffer(m_queues[dev_index], m_chrdatabufs[dev_index], CL_TRUE, 0, sizeof(cl_char)* (m_dicesizes[dev_index] + m_patternlen - 1), (cl_char *)chrdata.c_str() + m_totalanalyzedsize, 0, 0, 0);
			m_totalanalyzedsize += m_dicesizes[dev_index];
			m_worksizes.push_back(m_dicesizes[dev_index]);
#ifdef DEBUG
			cout << "Worksize: " << m_worksizes[dev_index] << ", Tailsize: " << tailsize << endl;
#endif
		}
	}
	cout << m_activedevnum << " devices selected to analyze..." << endl;

	return true;
}

void Cas_OFFinder::findPattern() {
	unsigned int dev_index;
	for (dev_index = 0; dev_index < m_activedevnum; dev_index++) {
		const size_t worksize = (size_t)m_worksizes[dev_index];
		oclEnqueueNDRangeKernel(m_queues[dev_index], m_finderkernels[dev_index], 1, 0, &worksize, 0, 0, 0, 0);
	}

	for (dev_index = 0; dev_index < m_activedevnum; dev_index++) {
		oclFinish(m_queues[dev_index]);
		m_locicnts.push_back(0);
		oclEnqueueReadBuffer(m_queues[dev_index], m_entrycountbufs[dev_index], CL_TRUE, 0, sizeof(cl_uint), &m_locicnts[dev_index], 0, 0, 0);
		if (m_locicnts[dev_index] > 0) {
			m_flags.push_back((cl_char *)malloc(sizeof(cl_char)* m_locicnts[dev_index]));
			oclEnqueueReadBuffer(m_queues[dev_index], m_flagbufs[dev_index], CL_TRUE, 0, sizeof(cl_char)*m_locicnts[dev_index], m_flags[dev_index], 0, 0, 0);

			m_mmcounts.push_back((cl_ushort *)malloc(sizeof(cl_ushort)* m_locicnts[dev_index] * 2)); // Maximum numbers of mismatch counts
			m_directions.push_back((cl_char *)malloc(sizeof(cl_char)* m_locicnts[dev_index] * 2));
			m_mmlocis.push_back((cl_uint *)malloc(sizeof(cl_uint)* m_locicnts[dev_index] * 2));

			m_mmlocibufs.push_back(oclCreateBuffer(m_contexts[dev_index], CL_MEM_WRITE_ONLY, sizeof(cl_uint)* m_locicnts[dev_index] * 2, 0));
			m_mmcountbufs.push_back(oclCreateBuffer(m_contexts[dev_index], CL_MEM_WRITE_ONLY, sizeof(cl_ushort)* m_locicnts[dev_index] * 2, 0));
			m_directionbufs.push_back(oclCreateBuffer(m_contexts[dev_index], CL_MEM_WRITE_ONLY, sizeof(cl_char)* m_locicnts[dev_index] * 2, 0));

			oclSetKernelArg(m_comparerkernels[dev_index], 2, sizeof(cl_mem), &m_mmlocibufs[dev_index]);
			oclSetKernelArg(m_comparerkernels[dev_index], 8, sizeof(cl_mem), &m_mmcountbufs[dev_index]);
			oclSetKernelArg(m_comparerkernels[dev_index], 9, sizeof(cl_mem), &m_directionbufs[dev_index]);
		}
		else {
			m_flags.push_back(0);
			m_mmcounts.push_back(0);
			m_directions.push_back(0);
			m_mmlocis.push_back(0);
			m_mmlocibufs.push_back(0);
			m_mmcountbufs.push_back(0);
			m_directionbufs.push_back(0);
		}
	}
}

void Cas_OFFinder::releaseLociinfo() {
	unsigned int dev_index;

	for (dev_index = 0; dev_index < m_activedevnum; dev_index++) {
		free((void *)m_mmcounts[dev_index]);
		free((void *)m_flags[dev_index]);
		free((void *)m_directions[dev_index]);
		free((void *)m_mmlocis[dev_index]);
	}
	m_directions.clear();
	m_mmlocis.clear();
	m_mmcounts.clear();
	m_locicnts.clear();
	clearbufvec(&m_mmlocibufs);
	m_flags.clear();
	clearbufvec(&m_mmcountbufs);
	clearbufvec(&m_directionbufs);
}

void Cas_OFFinder::indicate_mismatches(cl_char* seq, cl_char* comp) {
	unsigned int k;
	for (k = 0; k < m_patternlen; k++)
		if ((comp[k] == 'R' && (seq[k] == 'C' || seq[k] == 'T')) ||
			(comp[k] == 'Y' && (seq[k] == 'A' || seq[k] == 'G')) ||
			(comp[k] == 'K' && (seq[k] == 'A' || seq[k] == 'C')) ||
			(comp[k] == 'M' && (seq[k] == 'G' || seq[k] == 'T')) ||
			(comp[k] == 'W' && (seq[k] == 'C' || seq[k] == 'G')) ||
			(comp[k] == 'S' && (seq[k] == 'A' || seq[k] == 'T')) ||
			(comp[k] == 'H' && (seq[k] == 'G')) ||
			(comp[k] == 'B' && (seq[k] == 'A')) ||
			(comp[k] == 'V' && (seq[k] == 'T')) ||
			(comp[k] == 'D' && (seq[k] == 'C')) ||
			(comp[k] == 'A' && (seq[k] != 'A')) ||
			(comp[k] == 'G' && (seq[k] != 'G')) ||
			(comp[k] == 'C' && (seq[k] != 'C')) ||
			(comp[k] == 'T' && (seq[k] != 'T')))
			seq[k] += 32;
}

void Cas_OFFinder::compareAll(const char* outfilename) {
	unsigned int compcnt, i, j, dev_index;
	cl_uint zero = 0;

	cl_char *cl_compare = new cl_char[m_patternlen * 2];
	cl_int *cl_compare_flags = new cl_int[m_patternlen * 2];

	char *strbuf = new char[m_patternlen + 1]; strbuf[m_patternlen] = 0;

	for (compcnt = 0; compcnt < m_totalcompcount; compcnt++) {
		memcpy(cl_compare, m_compares[compcnt].c_str(), m_patternlen);
		memcpy(cl_compare + m_patternlen, m_compares[compcnt].c_str(), m_patternlen);
		set_complementary_sequence(cl_compare + m_patternlen, m_patternlen);
		set_seq_flags(cl_compare_flags, cl_compare, m_patternlen);
		set_seq_flags(cl_compare_flags + m_patternlen, cl_compare + m_patternlen, m_patternlen);

		for (dev_index = 0; dev_index < m_activedevnum; dev_index++) {
			if (m_locicnts[dev_index] > 0) {
				oclEnqueueWriteBuffer(m_queues[dev_index], m_comparebufs[dev_index], CL_FALSE, 0, sizeof(cl_char) * m_patternlen * 2, cl_compare, 0, 0, 0);
				oclEnqueueWriteBuffer(m_queues[dev_index], m_compareflagbufs[dev_index], CL_FALSE, 0, sizeof(cl_int) * m_patternlen * 2, cl_compare_flags, 0, 0, 0);
				oclEnqueueWriteBuffer(m_queues[dev_index], m_entrycountbufs[dev_index], CL_FALSE, 0, sizeof(cl_uint), &zero, 0, 0, 0);
				oclFinish(m_queues[dev_index]);
				oclSetKernelArg(m_comparerkernels[dev_index], 6, sizeof(cl_ushort), &m_thresholds[compcnt]);
				const size_t locicnts = m_locicnts[dev_index];
				oclEnqueueNDRangeKernel(m_queues[dev_index], m_comparerkernels[dev_index], 1, 0, &locicnts, 0, 0, 0, 0);
			}
		}

		unsigned long long loci;

		char comp_symbol[2] = { '+', '-' };

		ofstream fo(outfilename, ios::out | ios::app);
		unsigned long long localanalyzedsize = 0;
		unsigned int cnt = 0;
		unsigned int idx;
		for (dev_index = 0; dev_index < m_activedevnum; dev_index++) {
			if (m_locicnts[dev_index] > 0) {
				oclFinish(m_queues[dev_index]);
				oclEnqueueReadBuffer(m_queues[dev_index], m_entrycountbufs[dev_index], CL_TRUE, 0, sizeof(cl_uint), &cnt, 0, 0, 0);
				if (cnt > 0) {
					oclEnqueueReadBuffer(m_queues[dev_index], m_mmcountbufs[dev_index], CL_FALSE, 0, sizeof(cl_ushort)* cnt, m_mmcounts[dev_index], 0, 0, 0);
					oclEnqueueReadBuffer(m_queues[dev_index], m_directionbufs[dev_index], CL_FALSE, 0, sizeof(cl_char)* cnt, m_directions[dev_index], 0, 0, 0);
					oclEnqueueReadBuffer(m_queues[dev_index], m_mmlocibufs[dev_index], CL_FALSE, 0, sizeof(cl_uint)* cnt, m_mmlocis[dev_index], 0, 0, 0);
					oclFinish(m_queues[dev_index]);
					for (i = 0; i < cnt; i++) {
						loci = m_mmlocis[dev_index][i] + m_lasttotalanalyzedsize + localanalyzedsize;
						if (m_mmcounts[dev_index][i] <= m_thresholds[compcnt]) {
							strncpy(strbuf, (char *)(chrdata.c_str() + loci), m_patternlen);
							if (m_directions[dev_index][i] == '-') set_complementary_sequence((cl_char *)strbuf, m_patternlen);
							indicate_mismatches((cl_char*)strbuf, (cl_char*)m_compares[compcnt].c_str());
							for (j = 0; ((j < chrpos.size()) && (loci > chrpos[j])); j++) idx = j;
							fo << m_compares[compcnt] << "\t" << chrnames[idx] << "\t" << loci - chrpos[idx] << "\t" << strbuf << "\t" << m_directions[dev_index][i] << "\t" << m_mmcounts[dev_index][i] << endl;
						}
					}
				}
			}
			localanalyzedsize += m_worksizes[dev_index];
		}
		fo.close();
	}
	delete [] strbuf;
	delete [] cl_compare;
	delete [] cl_compare_flags;
}

void Cas_OFFinder::init_platforms() {
	oclGetPlatformIDs(MAX_PLATFORM_NUM, platforms, &platform_cnt);
	if (platform_cnt == 0) {
		cout << "No OpenCL platforms found. Check OpenCL installation!" << endl;
		exit(1);
	}
}
void Cas_OFFinder::print_usage() {
	unsigned int i, j;
	cout << "Cas-OFFinder v2.3 (" << __DATE__ << ")" << endl <<
		endl <<
		"Copyright (c) 2013 Jeongbin Park and Sangsu Bae" << endl <<
		"Website: http://github.com/snugel/cas-offinder" << endl <<
		endl <<
		"Usage: cas-offinder {input_file} {C|G|A} {output_file}" << endl <<
		"(C: using CPUs, G: using GPUs, A: using accelerators)" << endl <<
		endl <<
		"Example input file:" << endl <<
		"/var/chromosomes/human_hg19" << endl <<
		"NNNNNNNNNNNNNNNNNNNNNRG" << endl <<
		"GGCCGACCTGTCGCTGACGCNNN 5" << endl <<
		"CGCCAGCGTCAGCGACAGGTNNN 5" << endl <<
		"ACGGCGCCAGCGTCAGCGACNNN 5" << endl <<
		"GTCGCTGACGCTGGCGCCGTNNN 5" << endl <<
		endl <<
		"Available device list:" << endl;

	cl_device_id devices_per_platform[MAX_DEVICE_NUM];
	cl_uint device_cnt;
	cl_char devname[255] = { 0, };
	for (i = 0; i < platform_cnt; i++) {
		oclGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_CPU, MAX_DEVICE_NUM, devices_per_platform, &device_cnt);
		for (j = 0; j < device_cnt; j++) {
			oclGetDeviceInfo(devices_per_platform[j], CL_DEVICE_NAME, 255, &devname, 0);
			cout << "Type: CPU, '" << devname << "'" << endl;
		}
		oclGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_GPU, MAX_DEVICE_NUM, devices_per_platform, &device_cnt);
		for (j = 0; j < device_cnt; j++) {
			oclGetDeviceInfo(devices_per_platform[j], CL_DEVICE_NAME, 255, &devname, 0);
			cout << "Type: GPU, '" << devname << "'" << endl;
		}
		oclGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_ACCELERATOR, MAX_DEVICE_NUM, devices_per_platform, &device_cnt);
		for (j = 0; j < device_cnt; j++) {
			oclGetDeviceInfo(devices_per_platform[j], CL_DEVICE_NAME, 255, &devname, 0);
			cout << "Type: ACCELERATOR, '" << devname << "'" << endl;
		}
	}
}

vector<string> split(string const &input) {
	istringstream sbuffer(input);
	vector<string> ret((istream_iterator<string>(sbuffer)),
		istream_iterator<string>());
	return ret;	
}

void Cas_OFFinder::readInputFile(const char* inputfile) {
	unsigned int dev_index;
	string pattern, line;
	vector<string> sline;
	cl_uint zero = 0;

	ifstream fi(inputfile, ios::in);
	if (!fi.good()) {
		exit(0);
	}
	if (!fi.eof())
		getline(fi, chrdir);
	if (!fi.eof())
		getline(fi, pattern);
	transform(pattern.begin(), pattern.end(), pattern.begin(), ::toupper);
	while (getline(fi, line).good()) {
		if (line.empty()) break;
		sline = split(line);
		transform(sline[0].begin(), sline[0].end(), sline[0].begin(), ::toupper);
		m_compares.push_back(sline[0]);
		m_thresholds.push_back(atoi(sline[1].c_str()));
	}
	fi.close();

	m_totalcompcount = m_thresholds.size();
	m_patternlen = (cl_uint)(pattern.size());
	
	cl_char *cl_pattern = new cl_char[m_patternlen * 2]; 
	memcpy(cl_pattern, pattern.c_str(), m_patternlen);
	memcpy(cl_pattern + m_patternlen, pattern.c_str(), m_patternlen);
	set_complementary_sequence(cl_pattern+m_patternlen, m_patternlen);
	cl_int *cl_pattern_flags = new cl_int[m_patternlen * 2];
	set_seq_flags(cl_pattern_flags, cl_pattern, m_patternlen);
	set_seq_flags(cl_pattern_flags + m_patternlen, cl_pattern + m_patternlen, m_patternlen);
	
	for (dev_index = 0; dev_index < m_devnum; dev_index++) {
		m_patternbufs.push_back(oclCreateBuffer(m_contexts[dev_index], CL_MEM_READ_ONLY, sizeof(cl_char) * m_patternlen * 2, 0));
		m_patternflagbufs.push_back(oclCreateBuffer(m_contexts[dev_index], CL_MEM_READ_ONLY, sizeof(cl_int) * m_patternlen * 2, 0));
		oclEnqueueWriteBuffer(m_queues[dev_index], m_patternbufs[dev_index], CL_FALSE, 0, sizeof(cl_char) * m_patternlen * 2, cl_pattern, 0, 0, 0);
		oclEnqueueWriteBuffer(m_queues[dev_index], m_patternflagbufs[dev_index], CL_FALSE, 0, sizeof(cl_int) * m_patternlen * 2, cl_pattern_flags, 0, 0, 0);
		
		m_comparebufs.push_back(oclCreateBuffer(m_contexts[dev_index], CL_MEM_READ_ONLY, sizeof(cl_char) * m_patternlen * 2, 0));
		m_compareflagbufs.push_back(oclCreateBuffer(m_contexts[dev_index], CL_MEM_READ_ONLY, sizeof(cl_uint) * m_patternlen * 2, 0));

		m_entrycountbufs.push_back(oclCreateBuffer(m_contexts[dev_index], CL_MEM_READ_WRITE, sizeof(cl_uint), 0));
		oclEnqueueWriteBuffer(m_queues[dev_index], m_entrycountbufs[dev_index], CL_FALSE, 0, sizeof(cl_uint), &zero, 0, 0, 0);
		oclFinish(m_queues[dev_index]);

		oclSetKernelArg(m_finderkernels[dev_index], 1, sizeof(cl_mem), &m_patternbufs[dev_index]);
		oclSetKernelArg(m_finderkernels[dev_index], 2, sizeof(cl_mem), &m_patternflagbufs[dev_index]);
		oclSetKernelArg(m_finderkernels[dev_index], 3, sizeof(cl_uint), &m_patternlen);
		oclSetKernelArg(m_finderkernels[dev_index], 5, sizeof(cl_mem), &m_entrycountbufs[dev_index]);
		oclSetKernelArg(m_finderkernels[dev_index], 7, sizeof(cl_char) * m_patternlen * 2, 0);
		oclSetKernelArg(m_finderkernels[dev_index], 8, sizeof(cl_int) * m_patternlen * 2, 0);

		oclSetKernelArg(m_comparerkernels[dev_index], 3, sizeof(cl_mem), &m_comparebufs[dev_index]);
		oclSetKernelArg(m_comparerkernels[dev_index], 4, sizeof(cl_mem), &m_compareflagbufs[dev_index]);
		oclSetKernelArg(m_comparerkernels[dev_index], 5, sizeof(cl_uint), &m_patternlen);
		oclSetKernelArg(m_comparerkernels[dev_index], 10, sizeof(cl_mem), &m_entrycountbufs[dev_index]);
		oclSetKernelArg(m_comparerkernels[dev_index], 11, sizeof(cl_char) * m_patternlen * 2, 0);
		oclSetKernelArg(m_comparerkernels[dev_index], 12, sizeof(cl_int) * m_patternlen * 2, 0);
	}

	delete[] cl_pattern;
	delete[] cl_pattern_flags;
}
