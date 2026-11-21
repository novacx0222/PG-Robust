DROP TABLE IF EXISTS comp_cast_type CASCADE;
CREATE TABLE comp_cast_type (id integer, kind text);
\copy comp_cast_type FROM '/Users/xuanchen2026/RobDP/data/imdb/csv2/comp_cast_type.csv' WITH (FORMAT csv, NULL '')
ALTER TABLE comp_cast_type ADD PRIMARY KEY (id);
DROP TABLE IF EXISTS complete_cast CASCADE;
CREATE TABLE complete_cast (id integer, movie_id integer, subject_id integer, status_id integer);
\copy complete_cast FROM '/Users/xuanchen2026/RobDP/data/imdb/csv2/complete_cast.csv' WITH (FORMAT csv, NULL '')
ALTER TABLE complete_cast ADD PRIMARY KEY (id);
CREATE INDEX complete_cast_movie_id ON complete_cast (movie_id);
CREATE INDEX complete_cast_subject_id ON complete_cast (subject_id);
CREATE INDEX complete_cast_status_id ON complete_cast (status_id);
DROP TABLE IF EXISTS movie_info CASCADE;
CREATE TABLE movie_info (id integer, movie_id integer, info_type_id integer, info text, note text);
\copy movie_info FROM '/Users/xuanchen2026/RobDP/data/imdb/csv2/movie_info.csv' WITH (FORMAT csv, NULL '')
ALTER TABLE movie_info ADD PRIMARY KEY (id);
CREATE INDEX movie_info_movie_id ON movie_info (movie_id);
CREATE INDEX movie_info_info_type_id ON movie_info (info_type_id);
ANALYZE comp_cast_type;
ANALYZE complete_cast;
ANALYZE movie_info;
