#include <pocketsphinx.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pocketsphinx_internal.h"
#include "fsg_search_internal.h"
#include "test_macros.h"

int
main(int argc, char *argv[])
{
	ps_decoder_t *ps;
	cmd_ln_t *config;
	acmod_t *acmod;
	fsg_search_t *fsgs;
	ps_seg_t *seg;
	int32 score;
	clock_t c;
	int i;

	TEST_ASSERT(config =
		    cmd_ln_init(NULL, ps_args(), TRUE,
				"-hmm", MODELDIR "/hmm/wsj1",
				"-fsg", DATADIR "/goforward.fsg",
				"-dict", MODELDIR "/lm/turtle/turtle.dic",
				"-input_endian", "little",
				"-samprate", "16000", NULL));
	TEST_ASSERT(ps = ps_init(config));

	fsgs = (fsg_search_t *)ps->search;
	acmod = ps->acmod;

	setbuf(stdout, NULL);
	c = clock();
	for (i = 0; i < 5; ++i) {
		FILE *rawfh;
		int16 buf[2048];
		size_t nread;
		int16 const *bptr;
		char const *hyp;
		int nfr;

		TEST_ASSERT(rawfh = fopen(DATADIR "/goforward.raw", "rb"));
		TEST_EQUAL(0, acmod_start_utt(acmod));
		fsg_search_start(ps_search_base(fsgs));
		while (!feof(rawfh)) {
			nread = fread(buf, sizeof(*buf), 2048, rawfh);
			bptr = buf;
			while ((nfr = acmod_process_raw(acmod, &bptr, &nread, FALSE)) > 0) {
				while (fsg_search_step(ps_search_base(fsgs))) {
				}
			}
		}
		fsg_search_finish(ps_search_base(fsgs));
		hyp = fsg_search_hyp(ps_search_base(fsgs), &score);
		printf("FSG: %s (%d)\n", hyp, score);

		TEST_ASSERT(acmod_end_utt(acmod) >= 0);
		fclose(rawfh);
	}
	TEST_EQUAL(0, strcmp("GO FORWARD TEN METERS",
			     fsg_search_hyp(ps_search_base(fsgs), &score)));
	for (seg = ps_seg_iter(ps, &score); seg;
	     seg = ps_seg_next(seg)) {
		char const *word;
		int sf, ef;

		word = ps_seg_word(seg);
		ps_seg_frames(seg, &sf, &ef);
		printf("%s %d %d\n", word, sf, ef);
	}
	c = clock() - c;
	printf("5 * fsg search in %.2f sec\n", (double)c / CLOCKS_PER_SEC);
	ps_free(ps);

	return 0;
}
