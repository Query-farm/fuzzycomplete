#define DUCKDB_EXTENSION_MAIN

#include "fuzzycomplete_extension.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "utf8proc_wrapper.hpp"

// Include the declarations of things from Rust.
#include "rust.h"
#include "query_farm_telemetry.hpp"

namespace duckdb
{

	//! A single row of sql_auto_complete output, in the shape the DuckDB shell and the stock
	//! autocomplete extension use: (suggestion, suggestion_start, suggestion_type, suggestion_score, extra_char)
	struct FuzzyCompleteSuggestion
	{
		FuzzyCompleteSuggestion(string text_p, string type_p, uint64_t score_p, char extra_char_p)
				: text(std::move(text_p)), type(std::move(type_p)), score(score_p), extra_char(extra_char_p)
		{
		}

		//! The suggested text (including the extra character, if any)
		string text;
		//! The kind of suggestion: keyword, catalog, schema, table, table_function, column, scalar_function,
		//! file_name or directory (the names the shell understands)
		string type;
		//! Distance from the best fuzzy match: 0 for the best match(es), lower is better. Equal scores are
		//! treated as ties by the shell, which then lists the candidates instead of accepting the first one
		uint64_t score;
		//! The character that ends the suggestion (e.g. a space after a keyword, '.' after a schema, a quote after
		//! a file name), or '\0' if there is none
		char extra_char;
	};

	struct SQLFuzzyCompleteFunctionData : public TableFunctionData
	{
		explicit SQLFuzzyCompleteFunctionData(vector<FuzzyCompleteSuggestion> suggestions_p, idx_t start_pos)
				: suggestions(std::move(suggestions_p)), start_pos(start_pos)
		{
		}

		vector<FuzzyCompleteSuggestion> suggestions;
		idx_t start_pos;
	};

	struct SQLFuzzyCompleteData : public GlobalTableFunctionState
	{
		SQLFuzzyCompleteData() : offset(0)
		{
		}

		idx_t offset;
	};

	static constexpr const char *SUGGESTION_KEYWORD = "keyword";
	static constexpr const char *SUGGESTION_CATALOG = "catalog";
	static constexpr const char *SUGGESTION_SCHEMA = "schema";
	static constexpr const char *SUGGESTION_TABLE = "table";
	static constexpr const char *SUGGESTION_TABLE_FUNCTION = "table_function";
	static constexpr const char *SUGGESTION_COLUMN = "column";
	static constexpr const char *SUGGESTION_SCALAR_FUNCTION = "scalar_function";
	static constexpr const char *SUGGESTION_FILE_NAME = "file_name";
	static constexpr const char *SUGGESTION_DIRECTORY = "directory";

	struct FuzzyCompleteCandidate
	{
		FuzzyCompleteCandidate(string candidate_p, const char *type_p, int32_t score_bonus = 0, char extra_char = '\0')
				: candidate(std::move(candidate_p)), type(type_p), score_bonus(score_bonus), extra_char(extra_char)
		{
		}

		string candidate;
		//! The kind of suggestion (one of the SUGGESTION_* names)
		const char *type;
		//! The higher the score bonus, the more likely this candidate will be chosen
		int32_t score_bonus;
		//! The trailing character of the candidate text, if it has one
		char extra_char;
	};

	static constexpr idx_t DEFAULT_MAX_SUGGESTIONS = 20;
	static constexpr const char *FUZZYCOMPLETE_VERSION = "2026110701";

	static vector<FuzzyCompleteSuggestion> ComputeSuggestions(vector<FuzzyCompleteCandidate> available_suggestions,
																														const string &prefix,
																														const unordered_set<string> &extra_keywords,
																														idx_t max_results, bool add_quotes = false)
	{
		for (auto &kw : extra_keywords)
		{
			available_suggestions.emplace_back(kw, SUGGESTION_KEYWORD);
		}

		// Make sure all of the suggestions are unique (the first candidate with a given text wins), and remember
		// which candidate each entry handed to the matcher belongs to.
		std::vector<const char *> candidate_pool_pointers;
		std::vector<int32_t> candidate_pool_bonus;
		std::vector<idx_t> pool_to_candidate;
		candidate_pool_pointers.reserve(available_suggestions.size());
		candidate_pool_bonus.reserve(available_suggestions.size());
		pool_to_candidate.reserve(available_suggestions.size());
		unordered_map<string, idx_t> seen;
		for (idx_t i = 0; i < available_suggestions.size(); i++)
		{
			auto &candidate = available_suggestions[i];
			auto existing = seen.find(candidate.candidate);
			if (existing != seen.end())
			{
				// a duplicate keeps the first candidate's type, but the best bonus of all of them
				auto &bonus = candidate_pool_bonus[existing->second];
				bonus = MaxValue<int32_t>(bonus, candidate.score_bonus);
				continue;
			}
			seen.emplace(candidate.candidate, candidate_pool_pointers.size());
			candidate_pool_pointers.push_back(candidate.candidate.c_str());
			candidate_pool_bonus.push_back(candidate.score_bonus);
			pool_to_candidate.push_back(i);
		}

		// Never allocate more result slots than there are candidates, whatever max_suggestion_count says
		max_results = MinValue<idx_t>(max_results, candidate_pool_pointers.size());

		// Get the suggestions from rust
		std::vector<size_t> suggestion_results(max_results);
		std::vector<uint64_t> suggestion_scores(max_results);
		size_t actual_matches = 0;

		auto rc = perform_matches(
				candidate_pool_pointers.data(),
				candidate_pool_bonus.data(),
				candidate_pool_pointers.size(),
				prefix.c_str(),
				prefix.size(),
				max_results,
				suggestion_results.data(),
				suggestion_scores.data(),
				&actual_matches);
		if (rc != 0 || actual_matches > max_results)
		{
			throw InternalException("fuzzycomplete: the fuzzy matcher failed");
		}

		vector<FuzzyCompleteSuggestion> results;
		results.reserve(actual_matches);
		for (size_t i = 0; i < actual_matches; i++)
		{
			if (suggestion_results[i] >= pool_to_candidate.size())
			{
				throw InternalException("fuzzycomplete: matcher returned an unknown candidate");
			}
			auto &candidate = available_suggestions[pool_to_candidate[suggestion_results[i]]];
			// The matcher ranks the best match first; report the distance from it so that, like the stock
			// autocomplete extension, lower scores are better and equal scores are ties.
			uint64_t score = suggestion_scores[0] >= suggestion_scores[i] ? suggestion_scores[0] - suggestion_scores[i] : 0;
			string text = candidate.candidate;
			char extra_char = candidate.extra_char;

			if (add_quotes)
			{
				if (extra_keywords.find(text) == extra_keywords.end())
				{
					text = KeywordHelper::WriteOptionallyQuoted(text, '"', true);
				}
				else
				{
					text = text + " ";
					extra_char = ' ';
				}
			}
			results.emplace_back(std::move(text), candidate.type, score, extra_char);
		}
		return results;
	}

	static vector<string> InitialKeywords()
	{
		return vector<string>{"SELECT", "INSERT", "DELETE", "UPDATE", "CREATE", "DROP", "COPY",
													"ALTER", "WITH", "EXPORT", "BEGIN", "VACUUM", "PREPARE", "EXECUTE",
													"DEALLOCATE", "CALL", "ANALYZE", "EXPLAIN", "DESCRIBE", "SUMMARIZE", "LOAD",
													"CHECKPOINT", "ROLLBACK", "COMMIT", "CALL", "FROM", "PIVOT", "UNPIVOT"};
	}

	static vector<FuzzyCompleteCandidate> SuggestKeyword(ClientContext &context)
	{
		auto keywords = InitialKeywords();
		vector<FuzzyCompleteCandidate> result;
		for (auto &kw : keywords)
		{
			auto score = 0;
			if (kw == "SELECT")
			{
				score = 2;
			}
			if (kw == "FROM" || kw == "DELETE" || kw == "INSERT" || kw == "UPDATE")
			{
				score = 1;
			}
			result.emplace_back(kw + " ", SUGGESTION_KEYWORD, score, ' ');
		}
		return result;
	}

	static vector<reference<CatalogEntry>> GetAllTables(ClientContext &context, bool for_table_names)
	{
		vector<reference<CatalogEntry>> result;
		// scan all the schemas for tables and collect them and collect them
		// for column names we avoid adding internal entries, because it pollutes the auto-complete too much
		// for table names this is generally fine, however
		auto schemas = Catalog::GetAllSchemas(context);
		for (auto &schema_ref : schemas)
		{
			auto &schema = schema_ref.get();
			schema.Scan(context, CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry)
									{
			if (!entry.internal || for_table_names) {
				result.push_back(entry);
			} });
		};
		if (for_table_names)
		{
			for (auto &schema_ref : schemas)
			{
				auto &schema = schema_ref.get();
				schema.Scan(context, CatalogType::TABLE_FUNCTION_ENTRY,
										[&](CatalogEntry &entry)
										{ result.push_back(entry); });
			};
		}
		else
		{
			for (auto &schema_ref : schemas)
			{
				auto &schema = schema_ref.get();
				schema.Scan(context, CatalogType::SCALAR_FUNCTION_ENTRY,
										[&](CatalogEntry &entry)
										{ result.push_back(entry); });
			};
		}
		return result;
	}

	static vector<FuzzyCompleteCandidate> SuggestTableName(ClientContext &context)
	{
		vector<FuzzyCompleteCandidate> suggestions;
		auto all_entries = GetAllTables(context, true);

		auto default_database = DatabaseManager::GetDefaultDatabase(context).GetIdentifierName();

		auto default_schema = ClientData::Get(context).catalog_search_path->GetDefault().GetSchema().GetIdentifierName();

		// So really we should add the other catalog names.

		// Then the other schema names.
		// qualifier -> bonus: catalogs and schemas holding user entries rank like user tables (so the qualifier
		// comes before the names under it), those holding only built-in entries rank lower
		std::map<std::string, int32_t> seen_databases;
		std::map<std::string, int32_t> seen_schemas;

		// To get the list of available entries

		// There are a few possiblities.

		// same database same schema.
		for (auto &entry_ref : all_entries)
		{
			auto &entry = entry_ref.get();
			bool is_relation = entry.type == CatalogType::TABLE_ENTRY || entry.type == CatalogType::VIEW_ENTRY;
			// prioritize user-defined tables & views, then user-defined table macros, then everything built in
			int32_t bonus = entry.internal ? 0 : (is_relation ? 2 : 1);
			// table functions and table macros
			auto type = is_relation ? SUGGESTION_TABLE : SUGGESTION_TABLE_FUNCTION;

			// We should use the full path based on the calling context of the user.
			auto catalog_name = entry_ref.get().ParentCatalog().GetName().GetIdentifierName();
			auto schema_name = entry_ref.get().ParentSchema().name.GetIdentifierName();

			auto quoted_schema_name = KeywordHelper::WriteOptionallyQuoted(schema_name, '"', true);
			auto quoted_catalog_name = KeywordHelper::WriteOptionallyQuoted(catalog_name, '"', true);
			auto quoted_entry_name = KeywordHelper::WriteOptionallyQuoted(entry.name.GetIdentifierName(), '"', true);

			string prefix = quoted_catalog_name + "." + quoted_schema_name;
			int32_t qualifier_bonus = entry.internal ? 1 : 2;
			auto note_qualifier = [&](std::map<std::string, int32_t> &seen, const string &name) {
				auto &existing = seen[name];
				existing = MaxValue<int32_t>(existing, qualifier_bonus);
			};

			if (catalog_name != default_database)
			{
				note_qualifier(seen_databases, quoted_catalog_name);
				note_qualifier(seen_schemas, prefix);
				suggestions.emplace_back(prefix + "." + quoted_entry_name, type, bonus);
			}
			else
			{
				if (schema_name == default_schema)
				{
					suggestions.emplace_back(quoted_entry_name, type, bonus);
				}
				else
				{
					suggestions.emplace_back(quoted_schema_name + "." + quoted_entry_name, type, bonus);
					note_qualifier(seen_schemas, prefix);
				}
			}
		}

		for (auto &database : seen_databases)
		{
			suggestions.emplace_back(database.first + ".", SUGGESTION_CATALOG, database.second, '.');
		}

		for (auto &schema : seen_schemas)
		{
			suggestions.emplace_back(schema.first + ".", SUGGESTION_SCHEMA, schema.second, '.');
		}

		return suggestions;
	}

	static bool IsIdentifierChar(char c)
	{
		return StringUtil::CharacterIsAlphaNumeric(c) || c == '_' || static_cast<unsigned char>(c) >= 0x80;
	}

	//! The (lower-cased) identifier-like words of a query, used to find the tables it references
	static unordered_set<string> QueryWords(const string &sql)
	{
		unordered_set<string> words;
		string word;
		for (idx_t i = 0; i <= sql.size(); i++)
		{
			if (i < sql.size() && IsIdentifierChar(sql[i]))
			{
				word += sql[i];
			}
			else if (!word.empty())
			{
				words.insert(StringUtil::Lower(word));
				word.clear();
			}
		}
		return words;
	}

	//! Columns of the tables and views named in the query rank first, then other columns, then functions
	static vector<FuzzyCompleteCandidate> SuggestColumnName(ClientContext &context, const unordered_set<string> &query_words)
	{
		vector<FuzzyCompleteCandidate> suggestions;
		auto all_entries = GetAllTables(context, false);
		for (auto &entry_ref : all_entries)
		{
			auto &entry = entry_ref.get();
			int32_t column_bonus = 1;
			if (entry.type == CatalogType::TABLE_ENTRY || entry.type == CatalogType::VIEW_ENTRY)
			{
				if (query_words.count(StringUtil::Lower(entry.name.GetIdentifierName())))
				{
					column_bonus = 2;
				}
			}
			if (entry.type == CatalogType::TABLE_ENTRY)
			{
				auto &table = entry.Cast<TableCatalogEntry>();
				for (auto &col : table.GetColumns().Logical())
				{
					suggestions.emplace_back(col.GetName().GetIdentifierName(), SUGGESTION_COLUMN, column_bonus);
				}
			}
			else if (entry.type == CatalogType::VIEW_ENTRY)
			{
				auto &view = entry.Cast<ViewCatalogEntry>();
				for (auto &col : view.aliases)
				{
					suggestions.emplace_back(col.GetIdentifierName(), SUGGESTION_COLUMN, column_bonus);
				}
			}
			else
			{
				if (entry.name.GetIdentifierName().empty() || StringUtil::CharacterIsOperator(entry.name.GetIdentifierName()[0]))
				{
					continue;
				}
				suggestions.emplace_back(entry.name.GetIdentifierName(), SUGGESTION_SCALAR_FUNCTION);
			};
		}
		return suggestions;
	}

	static bool KnownExtension(const string &fname)
	{
		vector<string> known_extensions{".parquet", ".csv", ".tsv", ".csv.gz", ".tsv.gz", ".tbl"};
		for (auto &ext : known_extensions)
		{
			if (StringUtil::EndsWith(fname, ext))
			{
				return true;
			}
		}
		return false;
	}

	static vector<FuzzyCompleteCandidate> SuggestFileName(ClientContext &context, string &prefix, idx_t &last_pos)
	{
		auto &fs = FileSystem::GetFileSystem(context);
		string search_dir;
		D_ASSERT(last_pos >= prefix.size());
		auto is_path_absolute = fs.IsPathAbsolute(prefix);
		for (idx_t i = prefix.size(); i > 0; i--, last_pos--)
		{
			if (prefix[i - 1] == '/' || prefix[i - 1] == '\\')
			{
				search_dir = prefix.substr(0, i - 1);
				prefix = prefix.substr(i);
				break;
			}
		}
		if (search_dir.empty())
		{
			search_dir = is_path_absolute ? "/" : ".";
		}
		else
		{
			search_dir = fs.ExpandPath(search_dir);
		}
		vector<FuzzyCompleteCandidate> result;
		fs.ListFiles(search_dir, [&](const string &fname, bool is_dir)
								 {
		if (fname.empty() || !Utf8Proc::IsValid(fname.c_str(), fname.size())) {
			// a name that is not valid UTF-8 cannot be written in a SQL string (or returned as VARCHAR)
			return;
		}
		string suggestion;
		char extra_char;
		if (is_dir) {
			extra_char = fs.PathSeparator(fname)[0];
		} else {
			extra_char = '\'';
		}
		suggestion = fname + extra_char;
		int score = 0;
		if (is_dir && fname[0] != '.') {
			score = 2;
		}
		if (KnownExtension(fname)) {
			score = 1;
		}
		result.emplace_back(std::move(suggestion), is_dir ? SUGGESTION_DIRECTORY : SUGGESTION_FILE_NAME, score, extra_char); });
		return result;
	}

	//! Optional named parameters of sql_auto_complete, accepted for compatibility with the stock autocomplete
	//! extension (max_exact_suggestion_count is accepted but has no effect: fuzzy matching has no exact-only tail)
	struct FuzzyCompleteParameters
	{
		idx_t max_suggestion_count = DEFAULT_MAX_SUGGESTIONS;
		idx_t max_file_suggestion_count = DEFAULT_MAX_SUGGESTIONS;
	};

	enum class SuggestionState : uint8_t
	{
		SUGGEST_KEYWORD,
		SUGGEST_TABLE_NAME,
		SUGGEST_COLUMN_NAME,
		SUGGEST_FILE_NAME
	};

	static bool is_word_break(char c)
	{
		return StringUtil::CharacterIsOperator(c) && c != '.';
	}

	static duckdb::unique_ptr<SQLFuzzyCompleteFunctionData> GenerateSuggestions(ClientContext &context, const string &sql,
	                                                                             const FuzzyCompleteParameters &parameters)
	{
		// for auto-completion, we consider 4 scenarios
		// * there is nothing in the buffer, or only one word -> suggest a keyword
		// * the previous keyword is SELECT, WHERE, BY, HAVING, ... -> suggest a column name
		// * the previous keyword is FROM, INSERT, UPDATE ,... -> select a table name
		// * we are in a string constant -> suggest a filename
		// figure out which state we are in by doing a run through the query
		idx_t pos = 0;
		idx_t last_pos = 0;
		idx_t pos_offset = 0;
		bool seen_word = false;
		unordered_set<string> suggested_keywords;
		SuggestionState suggest_state = SuggestionState::SUGGEST_KEYWORD;
		case_insensitive_set_t column_name_keywords = {"SELECT", "WHERE", "BY", "HAVING", "QUALIFY",
																									 "LIMIT", "SET", "USING", "ON"};
		case_insensitive_set_t table_name_keywords = {"FROM", "JOIN", "INSERT", "UPDATE", "DELETE",
																									"ALTER", "DROP", "CALL", "DESCRIBE"};
		case_insensitive_map_t<unordered_set<string>> next_keyword_map;
		next_keyword_map["SELECT"] = {"FROM", "WHERE", "GROUP", "HAVING", "WINDOW", "ORDER", "LIMIT",
																	"QUALIFY", "SAMPLE", "VALUES", "UNION", "EXCEPT", "INTERSECT", "DISTINCT"};
		next_keyword_map["WITH"] = {"RECURSIVE", "SELECT", "AS"};
		next_keyword_map["INSERT"] = {"INTO", "VALUES", "SELECT", "DEFAULT"};
		next_keyword_map["DELETE"] = {"FROM", "WHERE", "USING"};
		next_keyword_map["UPDATE"] = {"SET", "WHERE"};
		next_keyword_map["CREATE"] = {"TABLE", "SCHEMA", "VIEW", "SEQUENCE", "MACRO", "FUNCTION", "SECRET", "TYPE"};
		next_keyword_map["DROP"] = next_keyword_map["CREATE"];
		next_keyword_map["ALTER"] = {"TABLE", "VIEW", "ADD", "DROP", "COLUMN", "SET", "TYPE", "DEFAULT", "DATA", "RENAME"};

	regular_scan:
		for (; pos < sql.size(); pos++)
		{
			if (sql[pos] == '\'')
			{
				pos++;
				last_pos = pos;
				goto in_string_constant;
			}
			if (sql[pos] == '"')
			{
				pos++;
				last_pos = pos;
				goto in_quotes;
			}
			if (sql[pos] == '-' && pos + 1 < sql.size() && sql[pos + 1] == '-')
			{
				goto in_comment;
			}
			if (sql[pos] == '/' && pos + 1 < sql.size() && sql[pos + 1] == '*')
			{
				if (seen_word)
				{
					// finish the word before the comment first; the scan resumes here afterwards
					goto process_word;
				}
				pos += 2;
				goto in_block_comment;
			}
			if (sql[pos] == ';')
			{
				// semicolon: restart suggestion flow
				suggest_state = SuggestionState::SUGGEST_KEYWORD;
				suggested_keywords.clear();
				last_pos = pos + 1;
				continue;
			}
			if (StringUtil::CharacterIsSpace(sql[pos]) || is_word_break(sql[pos]))
			{
				if (seen_word)
				{
					goto process_word;
				}
			}
			else
			{
				seen_word = true;
			}
		}
		goto standard_suggestion;
	in_comment:
		for (; pos < sql.size(); pos++)
		{
			if (sql[pos] == '\n' || sql[pos] == '\r')
			{
				pos++;
				last_pos = pos;
				goto regular_scan;
			}
		}
		// no suggestions inside comments
		return make_uniq<SQLFuzzyCompleteFunctionData>(vector<FuzzyCompleteSuggestion>(), 0);
	in_block_comment:
		for (; pos < sql.size(); pos++)
		{
			if (sql[pos] == '*' && pos + 1 < sql.size() && sql[pos + 1] == '/')
			{
				pos += 2;
				// a comment separates words, like whitespace
				last_pos = pos;
				goto regular_scan;
			}
		}
		// no suggestions inside comments
		return make_uniq<SQLFuzzyCompleteFunctionData>(vector<FuzzyCompleteSuggestion>(), 0);
	in_quotes:
		for (; pos < sql.size(); pos++)
		{
			if (sql[pos] == '"')
			{
				pos++;
				last_pos = pos;
				seen_word = true;
				goto regular_scan;
			}
		}
		pos_offset = 1;
		goto standard_suggestion;
	in_string_constant:
		for (; pos < sql.size(); pos++)
		{
			if (sql[pos] == '\'')
			{
				pos++;
				last_pos = pos;
				seen_word = true;
				goto regular_scan;
			}
		}
		suggest_state = SuggestionState::SUGGEST_FILE_NAME;
		goto standard_suggestion;
	process_word:
	{
		while ((last_pos < sql.size()) &&
					 (StringUtil::CharacterIsSpace(sql[last_pos]) || StringUtil::CharacterIsOperator(sql[last_pos])))
		{
			last_pos++;
		}
		auto next_word = sql.substr(last_pos, pos - last_pos);
		if (table_name_keywords.find(next_word) != table_name_keywords.end())
		{
			suggest_state = SuggestionState::SUGGEST_TABLE_NAME;
		}
		else if (column_name_keywords.find(next_word) != column_name_keywords.end())
		{
			suggest_state = SuggestionState::SUGGEST_COLUMN_NAME;
		}
		auto entry = next_keyword_map.find(next_word);
		if (entry != next_keyword_map.end())
		{
			suggested_keywords = entry->second;
		}
		else
		{
			suggested_keywords.erase(next_word);
		}
		if (std::all_of(next_word.begin(), next_word.end(), ::isdigit))
		{
			// Numbers are OK
			suggested_keywords.clear();
		}
		seen_word = false;
		last_pos = pos;
		goto regular_scan;
	}
	standard_suggestion:
		if (suggest_state != SuggestionState::SUGGEST_FILE_NAME)
		{
			while ((last_pos < sql.size()) &&
						 (StringUtil::CharacterIsSpace(sql[last_pos]) || is_word_break(sql[last_pos])))
			{
				last_pos++;
			}
		}
		auto last_word = sql.substr(last_pos, pos - last_pos);
		last_pos -= pos_offset;
		vector<FuzzyCompleteSuggestion> suggestions;

		switch (suggest_state)
		{
		case SuggestionState::SUGGEST_KEYWORD:
			suggestions = ComputeSuggestions(SuggestKeyword(context), last_word, suggested_keywords,
			                                 parameters.max_suggestion_count);
			break;
		case SuggestionState::SUGGEST_TABLE_NAME:
			suggestions = ComputeSuggestions(SuggestTableName(context), last_word, suggested_keywords,
			                                 parameters.max_suggestion_count, false);
			break;
		case SuggestionState::SUGGEST_COLUMN_NAME:
			suggestions = ComputeSuggestions(SuggestColumnName(context, QueryWords(sql)), last_word, suggested_keywords,
			                                 parameters.max_suggestion_count, true);
			break;
		case SuggestionState::SUGGEST_FILE_NAME:
			last_pos = pos;
			suggestions =
					ComputeSuggestions(SuggestFileName(context, last_word, last_pos), last_word, unordered_set<string>(),
			                       parameters.max_file_suggestion_count);
			break;
		default:
			throw InternalException("Unrecognized suggestion state");
		}
		if (last_pos > sql.size())
		{
			D_ASSERT(false);
			throw NotImplementedException("last_pos out of range");
		}
		if (!last_word.empty() && std::all_of(last_word.begin(), last_word.end(), ::isdigit))
		{
			// avoid giving auto-complete suggestion for digits
			suggestions.clear();
		}
		return make_uniq<SQLFuzzyCompleteFunctionData>(std::move(suggestions), last_pos);
	}

	static duckdb::unique_ptr<FunctionData> SQLFuzzyCompleteBind(ClientContext &context, TableFunctionBindInput &input,
																															 vector<LogicalType> &return_types, vector<Identifier> &names)
	{
		if (input.inputs[0].IsNull())
		{
			throw BinderException("sql_auto_complete first parameter cannot be NULL");
		}
		FuzzyCompleteParameters parameters;
		for (auto &param : input.named_parameters)
		{
			if (param.second.IsNull())
			{
				throw BinderException("sql_auto_complete %s cannot be NULL", param.first);
			}
			if (param.first == "max_suggestion_count")
			{
				parameters.max_suggestion_count = UBigIntValue::Get(param.second);
			}
			else if (param.first == "max_file_suggestion_count")
			{
				parameters.max_file_suggestion_count = UBigIntValue::Get(param.second);
			}
			else if (param.first == "max_exact_suggestion_count")
			{
				// accepted for compatibility with the stock autocomplete extension
			}
			else
			{
				throw InternalException("Unsupported parameter for SQL auto complete");
			}
		}

		// Same output shape as the stock autocomplete extension, which the DuckDB shell relies on
		names.emplace_back("suggestion");
		return_types.emplace_back(LogicalType::VARCHAR);

		names.emplace_back("suggestion_start");
		return_types.emplace_back(LogicalType::INTEGER);

		names.emplace_back("suggestion_type");
		return_types.emplace_back(LogicalType::VARCHAR);

		names.emplace_back("suggestion_score");
		return_types.emplace_back(LogicalType::UBIGINT);

		names.emplace_back("extra_char");
		return_types.emplace_back(LogicalType::VARCHAR);

		return GenerateSuggestions(context, StringValue::Get(input.inputs[0]), parameters);
	}

	unique_ptr<GlobalTableFunctionState> SQLFuzzyCompleteInit(ClientContext &context, TableFunctionInitInput &input)
	{
		return make_uniq<SQLFuzzyCompleteData>();
	}

	void SQLFuzzyCompleteFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output)
	{
		auto &bind_data = data_p.bind_data->Cast<SQLFuzzyCompleteFunctionData>();
		auto &data = data_p.global_state->Cast<SQLFuzzyCompleteData>();
		if (data.offset >= bind_data.suggestions.size())
		{
			// finished returning values
			return;
		}
		// start returning values
		// either fill up the chunk or return all the remaining columns
		idx_t count = 0;
		// suggestion, VARCHAR
		auto &suggestion = output.data[0];
		// suggestion_start, INTEGER
		auto &suggestion_start = output.data[1];
		// suggestion_type, VARCHAR
		auto &suggestion_type = output.data[2];
		// suggestion_score, UBIGINT
		auto &suggestion_score = output.data[3];
		// extra_char, VARCHAR
		auto &extra_char = output.data[4];
		while (data.offset < bind_data.suggestions.size() && count < STANDARD_VECTOR_SIZE)
		{
			auto &entry = bind_data.suggestions[data.offset++];

			suggestion.Append(Value(entry.text));
			suggestion_start.Append(Value::INTEGER(NumericCast<int32_t>(bind_data.start_pos)));
			suggestion_type.Append(Value(entry.type));
			suggestion_score.Append(Value::UBIGINT(entry.score));
			extra_char.Append(entry.extra_char == '\0' ? Value() : Value(string(1, entry.extra_char)));

			count++;
		}
	}

	static void LoadInternal(ExtensionLoader &loader)
	{
		TableFunction auto_complete_fun("sql_auto_complete",
		                                FunctionSignature().AddPositionalOnly("sql", LogicalType::VARCHAR),
		                                SQLFuzzyCompleteFunction, SQLFuzzyCompleteBind, SQLFuzzyCompleteInit);
		auto_complete_fun.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
			options.Add("max_suggestion_count", LogicalType::UBIGINT)
			    .Add("max_file_suggestion_count", LogicalType::UBIGINT)
			    .Add("max_exact_suggestion_count", LogicalType::UBIGINT);
		});
		CreateTableFunctionInfo info(std::move(auto_complete_fun));
		FunctionDescription description;
		description.description =
		    "Fuzzy auto-completion for the DuckDB shell: returns the keywords, tables, columns, functions or file names "
		    "that fuzzy match the last word of sql (e.g. 'cstmr' finds 'customer'), best match first. Replaces the "
		    "autocomplete extension's sql_auto_complete. Optional max_suggestion_count and max_file_suggestion_count "
		    "(default 20) limit the number of suggestions; max_exact_suggestion_count is accepted for compatibility "
		    "and ignored.";
		description.examples = {"SELECT suggestion, suggestion_type FROM sql_auto_complete('SELECT * FROM duckdb_tbl')",
		                        "SELECT * FROM sql_auto_complete('SLCT', max_suggestion_count := 5)"};
		description.categories = {"autocomplete", "utility"};
		info.descriptions.push_back(std::move(description));
		// The CLI preloads autocomplete. Replace its provider so LOAD fuzzycomplete
		// actually activates fuzzy matching, including when the function already exists.
		info.on_conflict = OnCreateConflict::REPLACE_ON_CONFLICT;
		loader.RegisterFunction(std::move(info));

		loader.SetDescription("Fuzzy matching auto-completion for the DuckDB shell");
		QueryFarmSendTelemetry(loader, "fuzzycomplete", FUZZYCOMPLETE_VERSION);
	}

	void FuzzycompleteExtension::Load(ExtensionLoader &loader)
	{
		LoadInternal(loader);
	}

	std::string FuzzycompleteExtension::Name()
	{
		return "fuzzycomplete";
	}

} // namespace duckdb
extern "C"
{

	DUCKDB_CPP_EXTENSION_ENTRY(fuzzycomplete, loader)
	{
		duckdb::LoadInternal(loader);
	}
}
