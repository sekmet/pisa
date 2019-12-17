#include <boost/algorithm/string.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/kruskal_min_spanning_tree.hpp>
#include <range/v3/iterator/operations.hpp>


#include <iostream>
#include <optional>

#include "mio/mmap.hpp"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"

#include "mappable/mapper.hpp"

#include "io.hpp"
#include "query/queries.hpp"
#include "taily.hpp"
#include "util/util.hpp"

#include "CLI/CLI.hpp"

using namespace pisa;
using Graph = boost::adjacency_list<boost::vecS,
                                    boost::vecS,
                                    boost::undirectedS,
                                    boost::no_property,
                                    boost::property<boost::edge_weight_t, float>>;
using Edge = boost::graph_traits<Graph>::edge_descriptor;

[[nodiscard]] auto estimate_pairwise_cutoff(taily::Query_Statistics const &stats,
                                            int ntop,
                                            double all) -> double
{
    auto const dist = taily::fit_distribution(stats.term_stats);
    double const p_c = std::min(1.0, ntop / all);
    return boost::math::quantile(complement(dist, p_c));
}

void real_intersection_thresholds(const std::string &taily_stats_filename,
                                  const std::vector<uint32_t> &intersections,
                                  const std::vector<Query> &queries,
                                  uint64_t k)
{
    std::ifstream ifs(taily_stats_filename);
    int64_t collection_size;
    ifs.read(reinterpret_cast<char *>(&collection_size), sizeof(collection_size));
    int64_t term_num;
    ifs.read(reinterpret_cast<char *>(&term_num), sizeof(term_num));

    std::vector<taily::Feature_Statistics> stats;
    for (int i = 0; i < term_num; ++i) {
        stats.push_back(taily::Feature_Statistics::from_stream(ifs));
    }

    for (auto&& [idx, query] : queries | ranges::views::enumerate) {
        auto terms = query.terms;
        double threshold = 0;
        if (terms.size()) {
            double all = intersections[idx];
            std::vector<taily::Feature_Statistics> term_stats;
            for (auto &&t : terms) {
                term_stats.push_back(stats[t]);
            }
            taily::Query_Statistics query_stats{term_stats, collection_size};
            threshold = estimate_pairwise_cutoff(query_stats, k, all);
        }
        std::cout << threshold << '\n';
    }
}

void pairwise_thresholds(const std::string &taily_stats_filename,
                         const std::map<std::set<uint32_t>, uint32_t> &bigrams_stats,
                         const std::vector<Query> &queries,
                         uint64_t k)
{
    std::ifstream ifs(taily_stats_filename);
    int64_t collection_size;
    ifs.read(reinterpret_cast<char *>(&collection_size), sizeof(collection_size));
    int64_t term_num;
    ifs.read(reinterpret_cast<char *>(&term_num), sizeof(term_num));

    std::vector<taily::Feature_Statistics> stats;
    for (int i = 0; i < term_num; ++i) {
        stats.push_back(taily::Feature_Statistics::from_stream(ifs));
    }

    for (auto const &query : queries) {
        auto terms = query.terms;
        double threshold = 0;
        if (terms.size()) {
            int num_nodes = terms.size();
            std::vector<std::pair<int, int>> edge_array;
            std::vector<float> weights;
            for (size_t i = 0; i < terms.size(); ++i) {
                for (size_t j = i + 1; j < terms.size(); ++j) {
                    auto it = bigrams_stats.find({terms[i], terms[j]});
                    if (it != bigrams_stats.end()) {
                        edge_array.emplace_back(i, j);
                        double t1 = stats[terms[i]].frequency / collection_size;
                        double t2 = stats[terms[j]].frequency / collection_size;
                        double t12 = it->second / collection_size;
                        weights.push_back(-t12 / (t1 * t2));
                    }
                }
            }
            Graph g(edge_array.begin(), edge_array.end(), weights.begin(), num_nodes);
            std::vector<Edge> spanning_tree;
            boost::kruskal_minimum_spanning_tree(g, std::back_inserter(spanning_tree));

            double all = 1;

            std::vector<taily::Feature_Statistics> term_stats;
            for (auto &&t : terms) {
                term_stats.push_back(stats[t]);
            }
            taily::Query_Statistics query_stats{term_stats, collection_size};
            double const any = taily::any(query_stats);

            for (std::vector<Edge>::iterator ei = spanning_tree.begin(); ei != spanning_tree.end();
                 ++ei) {
                auto bi_it = bigrams_stats.find({terms[source(*ei, g)], terms[target(*ei, g)]});
                double t1 = stats[terms[source(*ei, g)]].frequency / any;
                double t2 = stats[terms[target(*ei, g)]].frequency / any;
                all *= (bi_it->second / any) / (t1 * t2);
            }

            for (auto &&t : terms) {
                all *= stats[t].frequency / any;
            }
            all *= any;
            threshold = estimate_pairwise_cutoff(query_stats, k, all);
        }
        std::cout << threshold << '\n';
    }
}

void disjunctive_taily(const std::string &taily_stats_filename,
                const std::vector<Query> &queries,
                uint64_t k)
{
    std::ifstream ifs(taily_stats_filename);
    int64_t collection_size;
    ifs.read(reinterpret_cast<char *>(&collection_size), sizeof(collection_size));
    int64_t term_num;
    ifs.read(reinterpret_cast<char *>(&term_num), sizeof(term_num));

    std::vector<taily::Feature_Statistics> stats;
    for (int i = 0; i < term_num; ++i) {
        stats.push_back(taily::Feature_Statistics::from_stream(ifs));
    }

    for (auto const &query : queries) {
        auto terms = query.terms;
        double threshold = 0;
        if (terms.size()) {
            std::vector<taily::Feature_Statistics> term_stats;
            for (auto &&t : terms) {
                term_stats.push_back(stats[t]);
            }
            taily::Query_Statistics query_stats{term_stats, collection_size};
            double any = taily::any(query_stats);

            double sum1 = 0.0;
            double sum2 = 0.0;
            double variance = 0;
            double expected_value = 0;

            for(auto&& t : term_stats) {
                sum1 += t.expected_value * t.frequency;
                sum2 += (t.variance + std::pow(t.expected_value, 2) ) * t.frequency;
            }

            for (size_t i = 0; i < term_stats.size(); i++) {
                for (size_t j = i + 1; j < term_stats.size(); j++) {
                    sum2 += 2 * (term_stats[i].frequency / collection_size) * term_stats[j].frequency * term_stats[i].expected_value
                            * term_stats[j].expected_value;
                }
            }

            if (any * sum2 >= sum1 * sum1) {
                expected_value = sum1 / any;
                variance = (any * sum2 - sum1 * sum1) / (any * any);
            } else {
                any = sum1 * sum1 / sum2;
                variance = 0.0;
                expected_value = sum1 / any;
            }


            double epsilon = std::numeric_limits<double>::epsilon();
            variance = std::max(epsilon, variance);
            const double k = std::pow(expected_value, 2.0) / variance;
            const double theta = variance / expected_value;
            auto const dist = boost::math::gamma_distribution<>(k, theta);

            double const p_c = std::min(1.0, k / any);
            threshold = boost::math::quantile(complement(dist, p_c));
        }
        std::cout << threshold << '\n';
    }

}


void thresholds(const std::string &taily_stats_filename,
                const std::vector<Query> &queries,
                uint64_t k)
{
    std::ifstream ifs(taily_stats_filename);
    int64_t collection_size;
    ifs.read(reinterpret_cast<char *>(&collection_size), sizeof(collection_size));
    int64_t term_num;
    ifs.read(reinterpret_cast<char *>(&term_num), sizeof(term_num));

    std::vector<taily::Feature_Statistics> stats;
    for (int i = 0; i < term_num; ++i) {
        stats.push_back(taily::Feature_Statistics::from_stream(ifs));
    }

    for (auto const &query : queries) {
        auto terms = query.terms;
        double threshold = 0;
        if (terms.size()) {
            std::vector<taily::Feature_Statistics> term_stats;
            for (auto &&t : terms) {
                term_stats.push_back(stats[t]);
            }
            taily::Query_Statistics query_stats{term_stats, collection_size};
            threshold = taily::estimate_cutoff(query_stats, k);
        }
        std::cout << threshold << '\n';
    }
}

int main(int argc, const char **argv)
{
    spdlog::drop("");
    spdlog::set_default_logger(spdlog::stderr_color_mt(""));

    std::string taily_stats_filename;
    std::optional<std::string> terms_file;
    std::optional<std::string> query_filename;
    std::optional<std::string> stemmer = std::nullopt;
    bool is_disjunctive_taily = false;
    uint64_t k = configuration::get().k;

    std::optional<std::string> pairwise_filename;
    std::optional<std::string> intersection_filename;

    CLI::App app{"A tool for predicting thresholds for queries using Taily."};
    app.set_config("--config", "", "Configuration .ini file", false);
    app.add_option("-t,--taily", taily_stats_filename, "Taily stats filename")->required();
    app.add_option("-q,--query", query_filename, "Queries filename");
    app.add_option("-k", k, "k value");
    app.add_flag("-d, --disjunctive", is_disjunctive_taily, "Disjunctive filename");
    app.add_option("-p, --pairwise", pairwise_filename, "Pairwise filename");
    app.add_option("-i, --intersection", intersection_filename, "Intersection filename");
    auto *terms_opt =
        app.add_option("--terms", terms_file, "Text file with terms in separate lines");
    app.add_option("--stemmer", stemmer, "Stemmer type")->needs(terms_opt);
    CLI11_PARSE(app, argc, argv);

    std::vector<Query> queries;
    auto parse_query = resolve_query_parser(queries, terms_file, std::nullopt, stemmer);
    if (query_filename) {
        std::ifstream is(*query_filename);
        io::for_each_line(is, parse_query);
    } else {
        io::for_each_line(std::cin, parse_query);
    }

    if (pairwise_filename) {
        std::map<std::set<uint32_t>, uint32_t> bigrams_stats;
        std::ifstream bigrams_fs(*pairwise_filename);
        auto term_processor = TermProcessor(terms_file, std::nullopt, stemmer);
        std::string line;
        while (std::getline(bigrams_fs, line)) {
            std::vector<std::string> tokens;
            boost::split(tokens, line, boost::is_any_of("\t"));
            if (tokens.size() == 3) {
                bigrams_stats.insert({{*term_processor(tokens[0]), *term_processor(tokens[1])},
                                      std::stoi(tokens[2])});
            }
        }
        spdlog::info("Number of bigrams: {}", bigrams_stats.size());

        pairwise_thresholds(taily_stats_filename, bigrams_stats, queries, k);
    } else if (is_disjunctive_taily) {
        disjunctive_taily(taily_stats_filename, queries, k);
    } else if (intersection_filename) {
        std::vector<uint32_t> intersections;
        std::ifstream intersection_fs(*intersection_filename);
        std::string line;
        while (std::getline(intersection_fs, line)) {
            intersections.push_back(std::stoi(line));
        }
        real_intersection_thresholds(taily_stats_filename, intersections, queries, k);
    } else {
        thresholds(taily_stats_filename, queries, k);
    }
}
