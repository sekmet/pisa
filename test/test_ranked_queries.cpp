#define CATCH_CONFIG_MAIN

#include <functional>
#include <utility>
#include <vector>

#include <catch2/catch.hpp>
#include "test_common.hpp"

#include "accumulator/lazy_accumulator.hpp"
#include "cursor/block_max_scored_cursor.hpp"
#include "cursor/max_scored_cursor.hpp"
#include "cursor/scored_cursor.hpp"
#include "index_types.hpp"
#include "pisa_config.hpp"
#include "query/algorithm.hpp"
#include "query/queries.hpp"

using namespace pisa;

template <typename Index>
struct IndexData {
    static std::unordered_map<std::string, std::unique_ptr<IndexData>> data;

    explicit IndexData(std::string const &scorer_name)
        : collection(PISA_SOURCE_DIR "/test/test_data/test_collection"),
          document_sizes(PISA_SOURCE_DIR "/test/test_data/test_collection.sizes"),
          wdata(document_sizes.begin()->begin(),
                collection.num_docs(),
                collection,
                scorer_name,
                BlockSize(FixedBlock()))

    {
        typename Index::builder builder(collection.num_docs(), params);
        for (auto const &plist : collection) {
            uint64_t freqs_sum =
                std::accumulate(plist.frequencies.begin(), plist.frequencies.end(), uint64_t(0));
            builder.add_posting_list(plist.documents.size(),
                                     plist.documents.begin(),
                                     plist.frequencies.begin(),
                                     freqs_sum);
        }
        builder.build(index);

        auto process_term = [](auto str) { return std::stoi(str); };

        term_id_vec q;
        std::ifstream qfile(PISA_SOURCE_DIR "/test/test_data/queries");
        auto push_query = [&](std::string const &query_line) {
            queries.push_back(parse_query(query_line, process_term, {}));
        };
        io::for_each_line(qfile, push_query);

        std::string t;
    }

    [[nodiscard]] static auto get(std::string const &s_name)
    {
        if (IndexData::data.find(s_name) == IndexData::data.end()) {
            IndexData::data[s_name] = std::make_unique<IndexData<Index>>(s_name);
        }
        return IndexData::data[s_name].get();
    }

    global_parameters params;
    BinaryFreqCollection collection;
    BinaryCollection document_sizes;
    Index index;
    std::vector<Query> queries;
    wand_data<wand_data_raw> wdata;
};

template <typename Index>
std::unordered_map<std::string, unique_ptr<IndexData<Index>>> IndexData<Index>::data = {};

template <typename Acc>
class ranked_or_taat_query_acc : public ranked_or_taat_query {
   public:
    using ranked_or_taat_query::ranked_or_taat_query;

    template <typename Cursor>
    uint64_t operator()(gsl::span<Cursor> cursors, uint64_t max_docid)
    {
        Acc accumulator(max_docid);
        return ranked_or_taat_query::operator()(cursors, max_docid, std::move(accumulator));
    }
};

template <typename T>
class range_query_128 : public range_query<T> {
   public:
    using range_query<T>::range_query;

    template <typename Cursor>
    uint64_t operator()(gsl::span<Cursor> cursors, uint64_t max_docid)
    {
        return range_query<T>::operator()(cursors, max_docid, 128);
    }
};

TEMPLATE_TEST_CASE("Ranked query test",
                   "[query][ranked][integration]",
                   ranked_or_taat_query_acc<SimpleAccumulator>,
                   ranked_or_taat_query_acc<LazyAccumulator<4>>,
                   wand_query,
                   maxscore_query,
                   block_max_wand_query,
                   block_max_maxscore_query,
                   range_query_128<ranked_or_taat_query_acc<SimpleAccumulator>>,
                   range_query_128<ranked_or_taat_query_acc<LazyAccumulator<4>>>,
                   range_query_128<wand_query>,
                   range_query_128<maxscore_query>,
                   range_query_128<block_max_wand_query>,
                   range_query_128<block_max_maxscore_query>)
{
    for (auto &&s_name : {"bm25"}) {
        auto data = IndexData<single_index>::get(s_name);
        TestType op_q(10);
        ranked_or_query or_q(10);

        with_scorer(s_name, data->wdata, [&](auto scorer) {
            for (auto const &q : data->queries) {
                auto or_cursors =
                    make_block_max_scored_cursors(data->index, data->wdata, scorer, q);
                auto op_cursors =
                    make_block_max_scored_cursors(data->index, data->wdata, scorer, q);
                or_q(gsl::make_span(or_cursors), data->index.num_docs());
                op_q(gsl::make_span(op_cursors), data->index.num_docs());
                REQUIRE(or_q.topk().size() == op_q.topk().size());
                for (size_t i = 0; i < or_q.topk().size(); ++i) {
                    REQUIRE(or_q.topk()[i].first == Approx(op_q.topk()[i].first).epsilon(0.1));
                }
            }
        });
    }
}

TEMPLATE_TEST_CASE("Ranked AND query test",
                   "[query][ranked][integration]",
                   block_max_ranked_and_query)
{
    for (auto &&s_name : {"bm25"}) {

        auto data = IndexData<single_index>::get(s_name);
        TestType op_q(10);
        ranked_and_query and_q(10);

        with_scorer(s_name, data->wdata, [&](auto scorer) {
            for (auto const &q : data->queries) {
                auto and_cursors = make_scored_cursors(data->index, scorer, q);
                and_q(gsl::make_span(and_cursors), data->index.num_docs());
                auto op_cursors =
                    make_block_max_scored_cursors(data->index, data->wdata, scorer, q);
                op_q(gsl::make_span(op_cursors), data->index.num_docs());
                REQUIRE(and_q.topk().size() == op_q.topk().size());
                for (size_t i = 0; i < and_q.topk().size(); ++i) {
                    REQUIRE(and_q.topk()[i].first
                            == Approx(op_q.topk()[i].first).epsilon(0.1)); // tolerance is %
                                                                           // relative
                }
            }
        });
    }
}

TEST_CASE("Top k")
{
    for (auto &&s_name : {"bm25"}) {

        auto data = IndexData<single_index>::get(s_name);
        ranked_or_query or_10(10);
        ranked_or_query or_1(1);

        with_scorer(s_name, data->wdata, [&](auto scorer) {
            for (auto const &q : data->queries) {
                auto cursors = make_scored_cursors(data->index, scorer, q);
                or_10(gsl::make_span(cursors), data->index.num_docs());
                cursors = make_scored_cursors(data->index, scorer, q);
                or_1(gsl::make_span(cursors), data->index.num_docs());
                if (not or_10.topk().empty()) {
                    REQUIRE(not or_1.topk().empty());
                    REQUIRE(or_1.topk().front().first
                            == Approx(or_10.topk().front().first).epsilon(0.1));
                }
            }
        });
    }
}
