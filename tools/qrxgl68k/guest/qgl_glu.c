/* The GLU functions Blender 2.79 calls, on top of the door's GL. Formulas as in the GLU 1.3
 * specification (and SGI's reference implementation). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

void glMultMatrixd(const double *m);
void glTranslated(double x, double y, double z);
void glOrtho(double l, double r, double b, double t, double n, double f);

void gluOrtho2D(double left, double right, double bottom, double top)
{
	glOrtho(left, right, bottom, top, -1.0, 1.0);
}

void gluPerspective(double fovy, double aspect, double zNear, double zFar)
{
	double m[16];
	double rad = fovy / 2.0 * M_PI / 180.0;
	double dz = zFar - zNear, s = sin(rad);
	if (dz == 0.0 || s == 0.0 || aspect == 0.0)
		return;
	double c = cos(rad) / s;
	memset(m, 0, sizeof m);
	m[0] = c / aspect;
	m[5] = c;
	m[10] = -(zFar + zNear) / dz;
	m[11] = -1.0;
	m[14] = -2.0 * zNear * zFar / dz;
	glMultMatrixd(m);
}

static void normalize(double v[3])
{
	double r = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (r != 0.0) {
		v[0] /= r; v[1] /= r; v[2] /= r;
	}
}

static void cross(const double a[3], const double b[3], double r[3])
{
	r[0] = a[1] * b[2] - a[2] * b[1];
	r[1] = a[2] * b[0] - a[0] * b[2];
	r[2] = a[0] * b[1] - a[1] * b[0];
}

void gluLookAt(double ex, double ey, double ez, double cx, double cy, double cz, double ux, double uy, double uz)
{
	double f[3] = {cx - ex, cy - ey, cz - ez}, up[3] = {ux, uy, uz}, s[3], u[3], m[16];
	normalize(f);
	cross(f, up, s);
	normalize(s);
	cross(s, f, u);
	memset(m, 0, sizeof m);
	m[0] = s[0]; m[4] = s[1]; m[8] = s[2];
	m[1] = u[0]; m[5] = u[1]; m[9] = u[2];
	m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
	m[15] = 1.0;
	glMultMatrixd(m);
	glTranslated(-ex, -ey, -ez);
}

void gluPickMatrix(double x, double y, double deltax, double deltay, const int viewport[4])
{
	if (deltax <= 0 || deltay <= 0)
		return;
	glTranslated((viewport[2] - 2 * (x - viewport[0])) / deltax, (viewport[3] - 2 * (y - viewport[1])) / deltay, 0);
	double m[16] = {viewport[2] / deltax, 0, 0, 0, 0, viewport[3] / deltay, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	glMultMatrixd(m);
}

/* ---- projection */
static void mul_vec(const double m[16], const double in[4], double out[4])
{
	for (int i = 0; i < 4; i++)
		out[i] = in[0] * m[0 * 4 + i] + in[1] * m[1 * 4 + i] + in[2] * m[2 * 4 + i] + in[3] * m[3 * 4 + i];
}

static void mul_mat(const double a[16], const double b[16], double r[16])
{
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
}

/* Gauss-Jordan with partial pivoting. */
static int invert(const double m[16], double out[16])
{
	double a[4][8];
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++) {
			a[i][j] = m[i * 4 + j];
			a[i][j + 4] = i == j;
		}
	for (int c = 0; c < 4; c++) {
		int p = c;
		for (int r = c + 1; r < 4; r++)
			if (fabs(a[r][c]) > fabs(a[p][c]))
				p = r;
		if (a[p][c] == 0.0)
			return 0;
		if (p != c)
			for (int j = 0; j < 8; j++) {
				double t = a[c][j]; a[c][j] = a[p][j]; a[p][j] = t;
			}
		double d = a[c][c];
		for (int j = 0; j < 8; j++)
			a[c][j] /= d;
		for (int r = 0; r < 4; r++)
			if (r != c) {
				double f = a[r][c];
				for (int j = 0; j < 8; j++)
					a[r][j] -= f * a[c][j];
			}
	}
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			out[i * 4 + j] = a[i][j + 4];
	return 1;
}

int gluProject(double ox, double oy, double oz, const double model[16], const double proj[16], const int vp[4],
               double *wx, double *wy, double *wz)
{
	double in[4] = {ox, oy, oz, 1.0}, t[4], o[4];
	mul_vec(model, in, t);
	mul_vec(proj, t, o);
	if (o[3] == 0.0)
		return 0;
	o[0] /= o[3]; o[1] /= o[3]; o[2] /= o[3];
	*wx = vp[0] + (o[0] * 0.5 + 0.5) * vp[2];
	*wy = vp[1] + (o[1] * 0.5 + 0.5) * vp[3];
	*wz = o[2] * 0.5 + 0.5;
	return 1;
}

int gluUnProject(double wx, double wy, double wz, const double model[16], const double proj[16], const int vp[4],
                 double *ox, double *oy, double *oz)
{
	double pm[16], inv[16], in[4], o[4];
	/* proj * model in GL's column-major layout. */
	mul_mat(model, proj, pm);
	if (!invert(pm, inv))
		return 0;
	in[0] = (wx - vp[0]) / vp[2] * 2.0 - 1.0;
	in[1] = (wy - vp[1]) / vp[3] * 2.0 - 1.0;
	in[2] = wz * 2.0 - 1.0;
	in[3] = 1.0;
	mul_vec(inv, in, o);
	if (o[3] == 0.0)
		return 0;
	*ox = o[0] / o[3]; *oy = o[1] / o[3]; *oz = o[2] / o[3];
	return 1;
}

/* ---- quadrics, after SGI's quad.c: fill, line and point styles; smooth, flat or no normals. */
enum { GLU_SMOOTH = 100000, GLU_FLAT = 100001, GLU_NONE = 100002, GLU_POINT = 100010, GLU_LINE = 100011,
       GLU_FILL = 100012, GLU_SILHOUETTE = 100013, GLU_OUTSIDE = 100020, GLU_INSIDE = 100021 };
enum { GL_POINTS = 0, GL_LINES = 1, GL_LINE_LOOP = 2, GL_LINE_STRIP = 3, GL_QUAD_STRIP = 8, GL_TRIANGLE_FAN = 6 };
void glBegin(unsigned mode);
void glEnd(void);
void glVertex3f(float x, float y, float z);
void glNormal3f(float x, float y, float z);

typedef struct GLUquadric { int style, normals, orient; } GLUquadric;

GLUquadric *gluNewQuadric(void)
{
	GLUquadric *q = malloc(sizeof *q);
	if (q) {
		q->style = GLU_FILL;
		q->normals = GLU_SMOOTH;
		q->orient = GLU_OUTSIDE;
	}
	return q;
}
void gluDeleteQuadric(GLUquadric *q) { free(q); }
void gluQuadricDrawStyle(GLUquadric *q, unsigned s) { if (q) q->style = (int)s; }
void gluQuadricNormals(GLUquadric *q, unsigned n) { if (q) q->normals = (int)n; }
void gluQuadricOrientation(GLUquadric *q, unsigned o) { if (q) q->orient = (int)o; }

static void normal(const GLUquadric *q, float x, float y, float z)
{
	if (q->normals == GLU_NONE)
		return;
	if (q->orient == GLU_INSIDE)
		glNormal3f(-x, -y, -z);
	else
		glNormal3f(x, y, z);
}

void gluCylinder(GLUquadric *q, double base, double top, double height, int slices, int stacks)
{
	if (!q || slices < 2 || stacks < 1)
		return;
	double len = sqrt(height * height + (base - top) * (base - top));
	float nz = len ? (float)((base - top) / len) : 0.0f, nxy = len ? (float)(height / len) : 1.0f;
	for (int j = 0; j < stacks; j++) {
		double z0 = height * j / stacks, z1 = height * (j + 1) / stacks;
		double r0 = base + (top - base) * j / stacks, r1 = base + (top - base) * (j + 1) / stacks;
		if (q->style == GLU_FILL) {
			glBegin(GL_QUAD_STRIP);
			for (int i = 0; i <= slices; i++) {
				double a = 2.0 * M_PI * (i == slices ? 0 : i) / slices;
				float c = (float)cos(a), s = (float)sin(a);
				normal(q, c * nxy, s * nxy, nz);
				if (q->orient == GLU_INSIDE) {
					glVertex3f((float)(r1 * c), (float)(r1 * s), (float)z1);
					glVertex3f((float)(r0 * c), (float)(r0 * s), (float)z0);
				} else {
					glVertex3f((float)(r0 * c), (float)(r0 * s), (float)z0);
					glVertex3f((float)(r1 * c), (float)(r1 * s), (float)z1);
				}
			}
			glEnd();
		} else {
			glBegin(q->style == GLU_POINT ? GL_POINTS : GL_LINES);
			for (int i = 0; i < slices; i++) {
				double a = 2.0 * M_PI * i / slices;
				float c = (float)cos(a), s = (float)sin(a);
				normal(q, c * nxy, s * nxy, nz);
				glVertex3f((float)(r0 * c), (float)(r0 * s), (float)z0);
				glVertex3f((float)(r1 * c), (float)(r1 * s), (float)z1);
			}
			glEnd();
			if (q->style != GLU_POINT) {
				glBegin(GL_LINE_LOOP);
				for (int i = 0; i < slices; i++) {
					double a = 2.0 * M_PI * i / slices;
					glVertex3f((float)(r0 * cos(a)), (float)(r0 * sin(a)), (float)z0);
				}
				glEnd();
			}
		}
	}
}

void gluSphere(GLUquadric *q, double radius, int slices, int stacks)
{
	if (!q || slices < 2 || stacks < 2)
		return;
	for (int j = 0; j < stacks; j++) {
		double t0 = M_PI * j / stacks, t1 = M_PI * (j + 1) / stacks;
		if (q->style == GLU_FILL)
			glBegin(GL_QUAD_STRIP);
		else
			glBegin(q->style == GLU_POINT ? GL_POINTS : GL_LINE_STRIP);
		for (int i = 0; i <= slices; i++) {
			double a = 2.0 * M_PI * (i == slices ? 0 : i) / slices;
			float x0 = (float)(sin(t0) * cos(a)), y0 = (float)(sin(t0) * sin(a)), z0 = (float)cos(t0);
			float x1 = (float)(sin(t1) * cos(a)), y1 = (float)(sin(t1) * sin(a)), z1 = (float)cos(t1);
			if (q->orient == GLU_INSIDE) {
				normal(q, x1, y1, z1); glVertex3f((float)radius * x1, (float)radius * y1, (float)radius * z1);
				normal(q, x0, y0, z0); glVertex3f((float)radius * x0, (float)radius * y0, (float)radius * z0);
			} else {
				normal(q, x0, y0, z0); glVertex3f((float)radius * x0, (float)radius * y0, (float)radius * z0);
				if (q->style == GLU_FILL) {
					normal(q, x1, y1, z1); glVertex3f((float)radius * x1, (float)radius * y1, (float)radius * z1);
				}
			}
		}
		glEnd();
	}
}

void gluDisk(GLUquadric *q, double inner, double outer, int slices, int loops)
{
	if (!q || slices < 2 || loops < 1)
		return;
	normal(q, 0, 0, 1);
	for (int j = 0; j < loops; j++) {
		double r0 = inner + (outer - inner) * j / loops, r1 = inner + (outer - inner) * (j + 1) / loops;
		if (q->style == GLU_FILL) {
			glBegin(GL_QUAD_STRIP);
			for (int i = 0; i <= slices; i++) {
				double a = 2.0 * M_PI * (i == slices ? 0 : i) / slices;
				float c = (float)cos(a), s = (float)sin(a);
				if (q->orient == GLU_INSIDE) {
					glVertex3f((float)(r0 * c), (float)(r0 * s), 0);
					glVertex3f((float)(r1 * c), (float)(r1 * s), 0);
				} else {
					glVertex3f((float)(r1 * c), (float)(r1 * s), 0);
					glVertex3f((float)(r0 * c), (float)(r0 * s), 0);
				}
			}
			glEnd();
		} else {
			glBegin(q->style == GLU_POINT ? GL_POINTS : GL_LINE_LOOP);
			for (int i = 0; i < slices; i++) {
				double a = 2.0 * M_PI * i / slices;
				glVertex3f((float)(r1 * cos(a)), (float)(r1 * sin(a)), 0);
			}
			glEnd();
		}
	}
}
