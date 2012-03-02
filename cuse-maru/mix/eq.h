#ifndef EQ_H__
#define EQ_H__

#include <stddef.h>

typedef struct maru_eq maru_eq_t;

maru_eq_t *maru_eq_new(size_t samples);
void maru_eq_free(maru_eq_t *eq);

void maru_eq_process(maru_eq_t *eq, float *data, size_t frames);

#endif

