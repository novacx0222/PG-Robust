DROP TABLE IF EXISTS company_name CASCADE;
CREATE TABLE company_name (id integer, name text, country_code text, imdb_id double precision, name_pcode_nf text, name_pcode_sf text, md5sum text);
\copy company_name FROM '/Users/xuanchen2026/RobDP/data/imdb/csv/company_name.csv' WITH (FORMAT csv, NULL '')
ALTER TABLE company_name ALTER COLUMN imdb_id TYPE integer USING imdb_id::integer;
ALTER TABLE company_name ADD PRIMARY KEY (id);
DROP TABLE IF EXISTS company_type CASCADE;
CREATE TABLE company_type (id integer, kind text);
\copy company_type FROM '/Users/xuanchen2026/RobDP/data/imdb/csv/company_type.csv' WITH (FORMAT csv, NULL '')
ALTER TABLE company_type ADD PRIMARY KEY (id);
DROP TABLE IF EXISTS info_type CASCADE;
CREATE TABLE info_type (id integer, info text);
\copy info_type FROM '/Users/xuanchen2026/RobDP/data/imdb/csv/info_type.csv' WITH (FORMAT csv, NULL '')
ALTER TABLE info_type ADD PRIMARY KEY (id);
DROP TABLE IF EXISTS keyword CASCADE;
CREATE TABLE keyword (id integer, keyword text, phonetic_code text);
\copy keyword FROM '/Users/xuanchen2026/RobDP/data/imdb/csv/keyword.csv' WITH (FORMAT csv, NULL '')
ALTER TABLE keyword ADD PRIMARY KEY (id);
DROP TABLE IF EXISTS kind_type CASCADE;
CREATE TABLE kind_type (id integer, kind text);
\copy kind_type FROM '/Users/xuanchen2026/RobDP/data/imdb/csv/kind_type.csv' WITH (FORMAT csv, NULL '')
ALTER TABLE kind_type ADD PRIMARY KEY (id);
DROP TABLE IF EXISTS link_type CASCADE;
CREATE TABLE link_type (id integer, link text);
\copy link_type FROM '/Users/xuanchen2026/RobDP/data/imdb/csv/link_type.csv' WITH (FORMAT csv, NULL '')
ALTER TABLE link_type ADD PRIMARY KEY (id);
DROP TABLE IF EXISTS movie_companies CASCADE;
CREATE TABLE movie_companies (id integer, movie_id integer, company_id integer, company_type_id integer, note text);
\copy movie_companies FROM '/Users/xuanchen2026/RobDP/data/imdb/csv/movie_companies.csv' WITH (FORMAT csv, NULL '')
ALTER TABLE movie_companies ADD PRIMARY KEY (id);
CREATE INDEX movie_companies_movie_id ON movie_companies (movie_id);
CREATE INDEX movie_companies_company_id ON movie_companies (company_id);
CREATE INDEX movie_companies_company_type_id ON movie_companies (company_type_id);
DROP TABLE IF EXISTS movie_info_idx CASCADE;
CREATE TABLE movie_info_idx (id integer, movie_id integer, info_type_id integer, info text, note integer);
\copy movie_info_idx FROM '/Users/xuanchen2026/RobDP/data/imdb/csv/movie_info_idx.csv' WITH (FORMAT csv, NULL '')
ALTER TABLE movie_info_idx ADD PRIMARY KEY (id);
CREATE INDEX movie_info_idx_movie_id ON movie_info_idx (movie_id);
CREATE INDEX movie_info_idx_info_type_id ON movie_info_idx (info_type_id);
DROP TABLE IF EXISTS movie_keyword CASCADE;
CREATE TABLE movie_keyword (id integer, movie_id integer, keyword_id integer);
\copy movie_keyword FROM '/Users/xuanchen2026/RobDP/data/imdb/csv/movie_keyword.csv' WITH (FORMAT csv, NULL '')
ALTER TABLE movie_keyword ADD PRIMARY KEY (id);
CREATE INDEX movie_keyword_movie_id ON movie_keyword (movie_id);
CREATE INDEX movie_keyword_keyword_id ON movie_keyword (keyword_id);
DROP TABLE IF EXISTS movie_link CASCADE;
CREATE TABLE movie_link (id integer, movie_id integer, linked_movie_id integer, link_type_id integer);
\copy movie_link FROM '/Users/xuanchen2026/RobDP/data/imdb/csv/movie_link.csv' WITH (FORMAT csv, NULL '')
ALTER TABLE movie_link ADD PRIMARY KEY (id);
CREATE INDEX movie_link_movie_id ON movie_link (movie_id);
CREATE INDEX movie_link_linked_movie_id ON movie_link (linked_movie_id);
CREATE INDEX movie_link_link_type_id ON movie_link (link_type_id);
DROP TABLE IF EXISTS title CASCADE;
CREATE TABLE title (id integer, title text, imdb_index text, kind_id integer, production_year double precision, imdb_id double precision, phonetic_code text, episode_of_id double precision, season_nr double precision, episode_nr double precision, series_years text, md5sum text);
\copy title FROM '/Users/xuanchen2026/RobDP/data/imdb/csv/title.csv' WITH (FORMAT csv, NULL '')
ALTER TABLE title ALTER COLUMN production_year TYPE integer USING production_year::integer;
ALTER TABLE title ALTER COLUMN imdb_id TYPE integer USING imdb_id::integer;
ALTER TABLE title ALTER COLUMN episode_of_id TYPE integer USING episode_of_id::integer;
ALTER TABLE title ALTER COLUMN season_nr TYPE integer USING season_nr::integer;
ALTER TABLE title ALTER COLUMN episode_nr TYPE integer USING episode_nr::integer;
ALTER TABLE title ADD PRIMARY KEY (id);
CREATE INDEX title_kind_id ON title (kind_id);
CREATE INDEX title_episode_of_id ON title (episode_of_id);
ANALYZE;
