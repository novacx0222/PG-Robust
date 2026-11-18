/*-------------------------------------------------------------------------
 *
 * robustprofile.c
 *	  Loading and binning RobDP error profiles (see robustprofile.h).
 *
 * The profile of querylet "key" is the file "<error_profile_path>/<key>.txt".
 * Each line holds two numbers, "sel_true sel_est", both in [0, 1]; empty
 * lines and lines starting with '#' are ignored.  Any other line is
 * reported with a WARNING (file and line number) and skipped -- v20 skipped
 * such lines and truncated long files silently.
 *
 * Profiles are loaded on first use and cached for the session.  A cached
 * profile is reloaded when its file changes (inode, size or modification
 * time), or when error_profile_path or error_bin_count changes.  So
 * changing a setting takes effect at the next planning, without restarting
 * the session (v20 only loaded profiles in the GUC's assign hook).
 *
 * Binning.  The points are sorted by (sel_est, x) -- x breaks ties so the
 * order, and therefore the bins, do not depend on the file order or on the
 * sort algorithm -- and cut into error_bin_count bins of equal count:
 * bin b holds the sorted points [end(b-1), end(b)) with
 *
 *		end(b) = llround((b + 1) * N / B),	end(B-1) = N.
 *
 * except that a boundary never splits points with equal sel_est: end(b) is
 * moved forward past them.  Otherwise a point in the later bin could never
 * be drawn, since every estimate equal to its sel_est is looked up in the
 * earlier bin (v20 had this problem; with a profile whose estimates are all
 * equal, every bin but the first was unreachable).  So bins can be empty,
 * when N < B or when there are many ties.
 *
 * An estimate s belongs to the first non-empty bin whose hi is >= s, or to
 * the last non-empty bin if s is larger than every point
 * (robust_profile_find_bin).  Every point of bin b satisfies
 * lo < sel_est <= hi.
 *
 * src/backend/optimizer/util/robustprofile.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <sys/stat.h>

#include "common/string.h"
#include "optimizer/robustprofile.h"
#include "storage/fd.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"

/* Longest profile key: "<alias>=<alias>" */
#define PROFILE_KEY_LEN		(2 * NAMEDATALEN)

/* At most this many malformed lines are reported one by one per file */
#define MAX_LINE_WARNINGS	5

typedef struct ProfileCacheEntry
{
	char		key[PROFILE_KEY_LEN];	/* hash key, must be first */
	char	   *path;			/* file the entry was loaded from */
	ino_t		ino;
	off_t		size;
	time_t		mtime;
	int			nbins;			/* error_bin_count it was binned for */
	RobustProfile *prof;		/* NULL if the file has no valid point */
} ProfileCacheEntry;

static HTAB *profile_cache = NULL;
static MemoryContext profile_cxt = NULL;

/*
 * A key is a relation alias or two aliases joined by '='.  Aliases are
 * identifiers chosen by whoever writes the query, so a quoted alias could
 * contain "../" and make the server open a file outside error_profile_path
 * (v20 did not check).  Only keys that stay inside the directory are
 * accepted.
 */
static bool
key_is_safe(const char *key)
{
	if (key[0] == '\0' || key[0] == '.')
		return false;
	if (strlen(key) >= PROFILE_KEY_LEN)
		return false;
	if (strchr(key, '/') != NULL || strchr(key, '\\') != NULL)
		return false;
	return true;
}

static void
init_cache(void)
{
	HASHCTL		ctl;

	profile_cxt = AllocSetContextCreate(TopMemoryContext,
										"RobDP error profiles",
										ALLOCSET_DEFAULT_SIZES);
	ctl.keysize = PROFILE_KEY_LEN;
	ctl.entrysize = sizeof(ProfileCacheEntry);
	ctl.hcxt = profile_cxt;
	profile_cache = hash_create("RobDP error profiles", 64, &ctl,
								HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
}

static void
free_entry_contents(ProfileCacheEntry *entry)
{
	if (entry->path)
		pfree(entry->path);
	if (entry->prof)
	{
		pfree(entry->prof->key);
		pfree(entry->prof->sel_est);
		pfree(entry->prof->x);
		pfree(entry->prof);
	}
	entry->path = NULL;
	entry->prof = NULL;
}

/* ------------------------------------------------------------------------
 * Parsing
 * ------------------------------------------------------------------------
 */

/* Parse "sel_true sel_est"; false if the line is not exactly two numbers */
static bool
parse_point(const char *line, double *sel_true, double *sel_est)
{
	char	   *end;

	errno = 0;
	*sel_true = strtod(line, &end);
	if (end == line || errno != 0)
		return false;
	line = end;
	*sel_est = strtod(line, &end);
	if (end == line || errno != 0)
		return false;
	while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
		end++;
	return *end == '\0';
}

static bool
valid_selectivity(double s)
{
	return !isnan(s) && s >= 0.0 && s <= 1.0;
}

typedef struct ProfilePoint
{
	double		sel_est;
	double		x;
} ProfilePoint;

static int
cmp_point(const void *a, const void *b)
{
	const ProfilePoint *pa = (const ProfilePoint *) a;
	const ProfilePoint *pb = (const ProfilePoint *) b;

	if (pa->sel_est != pb->sel_est)
		return pa->sel_est < pb->sel_est ? -1 : 1;
	if (pa->x != pb->x)
		return pa->x < pb->x ? -1 : 1;
	return 0;
}

static int
cmp_double(const void *a, const void *b)
{
	double		da = *(const double *) a;
	double		db = *(const double *) b;

	return (da > db) - (da < db);
}

/*
 * Read the points of a profile file.  Returns the number of valid points;
 * *points is allocated in the current memory context.
 */
static int
read_points(const char *path, ProfilePoint **points, int *nskipped)
{
	FILE	   *fp;
	StringInfoData line;
	int			lineno = 0;
	int			n = 0;
	int			cap = 64;
	ProfilePoint *pts;

	*nskipped = 0;
	fp = AllocateFile(path, "r");
	if (fp == NULL)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("RobDP: could not open error profile \"%s\": %m", path)));
		*points = NULL;
		return 0;
	}

	pts = palloc(sizeof(ProfilePoint) * cap);
	initStringInfo(&line);
	while (pg_get_line_buf(fp, &line))
	{
		const char *p = line.data;
		double		sel_true;
		double		sel_est;

		lineno++;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#')
			continue;

		if (!parse_point(p, &sel_true, &sel_est) ||
			!valid_selectivity(sel_true) || !valid_selectivity(sel_est))
		{
			if (*nskipped < MAX_LINE_WARNINGS)
				ereport(WARNING,
						(errmsg("RobDP: skipping invalid line %d of error profile \"%s\"",
								lineno, path),
						 errdetail("Each line must be \"sel_true sel_est\", two numbers in [0, 1].")));
			(*nskipped)++;
			continue;
		}

		if (n == cap)
		{
			cap *= 2;
			pts = repalloc_huge(pts, sizeof(ProfilePoint) * cap);
		}
		pts[n].sel_est = sel_est;
		pts[n].x = log(fmax(sel_true, ROBUST_PROFILE_MIN_SEL) /
					   fmax(sel_est, ROBUST_PROFILE_MIN_SEL));
		n++;
	}
	FreeFile(fp);
	pfree(line.data);

	if (*nskipped > MAX_LINE_WARNINGS)
		ereport(WARNING,
				(errmsg("RobDP: skipped %d invalid lines of error profile \"%s\" in all",
						*nskipped, path)));
	*points = pts;
	return n;
}

/* ------------------------------------------------------------------------
 * Binning
 * ------------------------------------------------------------------------
 */

/*
 * Silverman's rule of thumb for a Gaussian KDE of x[0 .. n-1]:
 *
 *		h = 0.9 * min(sd, IQR / 1.34) * n^(-1/5),
 *
 * with the quartiles taken as sorted[(n-1)/4] and sorted[3(n-1)/4]
 * (integer division).  Uses sd alone when the IQR is 0, and gives 0 (no
 * smoothing) when n < 2 or sd = 0.  The formula is v20's.
 */
static double
silverman_bandwidth(const double *x, int n, double sd)
{
	double	   *sorted;
	double		iqr;
	double		spread = sd;

	if (n < 2 || sd <= 0.0)
		return 0.0;
	sorted = palloc(sizeof(double) * n);
	memcpy(sorted, x, sizeof(double) * n);
	qsort(sorted, n, sizeof(double), cmp_double);
	iqr = sorted[(3 * (n - 1)) / 4] - sorted[(n - 1) / 4];
	pfree(sorted);
	if (iqr > 0.0 && iqr / 1.34 < sd)
		spread = iqr / 1.34;
	return 0.9 * spread * pow((double) n, -0.2);
}

static void
make_bins(RobustProfile *prof, int nbins)
{
	int			N = prof->npoints;
	int			start = 0;
	double		prev_hi = -INFINITY;

	prof->nbins = nbins;
	for (int b = 0; b < nbins; b++)
	{
		RobustProfileBin *bin = &prof->bins[b];
		int			end;
		double		sum = 0.0;
		double		ss = 0.0;

		end = (b == nbins - 1) ? N :
			(int) llround(((long long) (b + 1) * N) / (double) nbins);
		if (end > N)
			end = N;
		if (end < start)
			end = start;
		/* keep equal estimates together, see the header comment */
		while (end > start && end < N && prof->sel_est[end] == prof->sel_est[end - 1])
			end++;

		bin->first = start;
		bin->n = end - start;
		bin->lo = prev_hi;
		bin->hi = (bin->n > 0) ? prof->sel_est[end - 1] : prev_hi;
		bin->mean = bin->sd = bin->h = 0.0;
		if (bin->n > 0)
		{
			for (int i = start; i < end; i++)
				sum += prof->x[i];
			bin->mean = sum / bin->n;
			for (int i = start; i < end; i++)
			{
				/*
				 * Separate statements, so that the compiler cannot fuse the
				 * multiply-add into one FMA instruction: the statistics are
				 * then the same on every platform and compiler, and the test
				 * harness can reproduce them bit for bit.
				 */
				double		d = prof->x[i] - bin->mean;
				double		sq = d * d;

				ss += sq;
			}
			bin->sd = (bin->n > 1) ? sqrt(ss / (bin->n - 1)) : 0.0;
			bin->h = silverman_bandwidth(&prof->x[start], bin->n, bin->sd);
		}
		prev_hi = bin->hi;
		start = end;
	}
}

/*
 * robust_profile_find_bin
 *		The bin an estimate draws its errors from: the first non-empty bin
 *		whose largest estimate is >= sel_est, else the last non-empty bin.
 */
int
robust_profile_find_bin(const RobustProfile *prof, double sel_est)
{
	for (int b = 0; b < prof->nbins; b++)
	{
		if (prof->bins[b].n > 0 && sel_est <= prof->bins[b].hi)
			return b;
	}
	for (int b = prof->nbins - 1; b >= 0; b--)
	{
		if (prof->bins[b].n > 0)
			return b;
	}
	return -1;					/* not reached: a profile has points */
}

static void
dump_profile(const RobustProfile *prof, const char *path, int nskipped)
{
	RobustDump	d;
	StringInfoData bins;

	initStringInfo(&bins);
	appendStringInfoChar(&bins, '[');
	for (int b = 0; b < prof->nbins; b++)
	{
		const RobustProfileBin *bin = &prof->bins[b];

		if (b > 0)
			appendStringInfoChar(&bins, ',');
		appendStringInfo(&bins, "{\"first\":%d,\"n\":%d,\"lo\":", bin->first, bin->n);
		robust_json_number(&bins, bin->lo);
		appendStringInfoString(&bins, ",\"hi\":");
		robust_json_number(&bins, bin->hi);
		appendStringInfoString(&bins, ",\"mean\":");
		robust_json_number(&bins, bin->mean);
		appendStringInfoString(&bins, ",\"sd\":");
		robust_json_number(&bins, bin->sd);
		appendStringInfoString(&bins, ",\"h\":");
		robust_json_number(&bins, bin->h);
		appendStringInfoChar(&bins, '}');
	}
	appendStringInfoChar(&bins, ']');

	robust_dump_begin(&d, "profile");
	robust_dump_str(&d, "key", prof->key);
	robust_dump_str(&d, "path", path);
	robust_dump_int(&d, "n", prof->npoints);
	robust_dump_int(&d, "skipped", nskipped);
	robust_dump_raw(&d, "bins", bins.data);
	robust_dump_end(&d);
	pfree(bins.data);
}

/* Load and bin a profile file; NULL if it has no valid point */
static RobustProfile *
load_profile(const char *key, const char *path)
{
	ProfilePoint *pts;
	int			nskipped;
	int			n;
	RobustProfile *prof;

	n = read_points(path, &pts, &nskipped);
	if (n == 0)
	{
		if (pts != NULL)
		{
			ereport(WARNING,
					(errmsg("RobDP: error profile \"%s\" has no valid point and is ignored",
							path)));
			pfree(pts);
		}
		return NULL;
	}
	qsort(pts, n, sizeof(ProfilePoint), cmp_point);

	prof = MemoryContextAllocZero(profile_cxt, sizeof(RobustProfile));
	prof->key = MemoryContextStrdup(profile_cxt, key);
	prof->npoints = n;
	prof->sel_est = MemoryContextAllocHuge(profile_cxt, sizeof(double) * n);
	prof->x = MemoryContextAllocHuge(profile_cxt, sizeof(double) * n);
	for (int i = 0; i < n; i++)
	{
		prof->sel_est[i] = pts[i].sel_est;
		prof->x[i] = pts[i].x;
	}
	pfree(pts);

	make_bins(prof, error_bin_count);
	if (robust_debug_dump)
		dump_profile(prof, path, nskipped);
	return prof;
}

/*
 * robust_get_profile
 *		The error profile of querylet "key", or NULL if there is none.
 *
 * The result points into the session cache.  It stays valid until the next
 * call, which may reload it.
 */
const RobustProfile *
robust_get_profile(const char *key)
{
	char		path[MAXPGPATH];
	struct stat st;
	ProfileCacheEntry *entry;
	bool		found;

	if (error_profile_path == NULL || error_profile_path[0] == '\0')
		return NULL;
	if (!key_is_safe(key))
	{
		ereport(WARNING,
				(errmsg("RobDP: not looking up an error profile for \"%s\"", key),
				 errdetail("A profile key must not be empty, start with \".\" or contain \"/\" or \"\\\".")));
		return NULL;
	}
	snprintf(path, sizeof(path), "%s/%s.txt", error_profile_path, key);
	if (stat(path, &st) != 0)
	{
		if (errno != ENOENT && errno != ENOTDIR)
			ereport(WARNING,
					(errcode_for_file_access(),
					 errmsg("RobDP: could not stat error profile \"%s\": %m", path)));
		return NULL;
	}

	if (profile_cache == NULL)
		init_cache();
	entry = (ProfileCacheEntry *) hash_search(profile_cache, key, HASH_ENTER, &found);
	if (found &&
		strcmp(entry->path, path) == 0 &&
		entry->ino == st.st_ino &&
		entry->size == st.st_size &&
		entry->mtime == st.st_mtime &&
		entry->nbins == error_bin_count)
		return entry->prof;

	if (found)
		free_entry_contents(entry);
	else
	{
		entry->path = NULL;
		entry->prof = NULL;
	}

	/* fill the entry only once loading has succeeded, so an error leaves it empty */
	PG_TRY();
	{
		entry->prof = load_profile(key, path);
	}
	PG_CATCH();
	{
		hash_search(profile_cache, key, HASH_REMOVE, NULL);
		PG_RE_THROW();
	}
	PG_END_TRY();
	entry->path = MemoryContextStrdup(profile_cxt, path);
	entry->ino = st.st_ino;
	entry->size = st.st_size;
	entry->mtime = st.st_mtime;
	entry->nbins = error_bin_count;
	return entry->prof;
}
