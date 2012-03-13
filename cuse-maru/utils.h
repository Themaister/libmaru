#ifndef MARU_UTILS_H__
#define MARU_UTILS_H__

/** \ingroup internal
 * \brief Return next value that is power-of-two.
 */
static inline unsigned next_pot(unsigned v)
{
   v--;
   v |= v >> 1;
   v |= v >> 2;
   v |= v >> 4;
   v |= v >> 8;
   v |= v >> 16;
   v++;
   return v;
}

#endif

