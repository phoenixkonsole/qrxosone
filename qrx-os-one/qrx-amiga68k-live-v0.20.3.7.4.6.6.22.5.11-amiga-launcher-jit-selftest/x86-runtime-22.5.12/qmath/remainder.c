#include "qmath.h"
#include <math.h>

double remainder(double x, double y)
{
	int q;
	return remquo(x, y, &q);
}

/* qmath does not provide musl's optional weak_alias infrastructure. */
#ifdef weak_alias
weak_alias(remainder, drem);
#endif
