#pragma once

#include <chrono>

#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <fmt/format.h>
#include <tbb/task_group.h>
#include <tbb/task_scheduler_init.h>
#include <tl/optional.hpp>

#include "v1/index.hpp"
#include "v1/index_metadata.hpp"
#include "v1/progress_status.hpp"

namespace pisa::v1 {

template <typename... Writers>
struct IndexBuilder {
    explicit IndexBuilder(Writers... writers) : m_writers(std::move(writers)...) {}

    template <typename Fn>
    void operator()(Encoding document_encoding, Encoding payload_encoding, Fn fn)
    {
        auto run = [&](auto &&dwriter, auto &&pwriter) -> bool {
            if (std::decay_t<decltype(dwriter)>::encoding() == document_encoding
                && std::decay_t<decltype(pwriter)>::encoding() == payload_encoding) {
                fn(dwriter, pwriter);
                return true;
            }
            return false;
        };
        bool success = std::apply(
            [&](Writers... dwriters) {
                auto with_document_writer = [&](auto dwriter) {
                    return std::apply(
                        [&](Writers... pwriters) { return (run(dwriter, pwriters) || ...); },
                        m_writers);
                };
                return (with_document_writer(dwriters) || ...);
            },
            m_writers);
        if (not success) {
            throw std::domain_error("Unknown posting encoding");
        }
    }

   private:
    std::tuple<Writers...> m_writers;
};

template <typename... Writers>
auto make_index_builder(Writers... writers)
{
    return IndexBuilder<Writers...>(std::move(writers)...);
}

template <typename CollectionIterator>
auto compress_batch(CollectionIterator first,
                    CollectionIterator last,
                    std::ofstream &dout,
                    std::ofstream &fout,
                    Writer<DocId> document_writer,
                    Writer<Frequency> frequency_writer,
                    tl::optional<ProgressStatus &> bar)
    -> std::tuple<std::vector<std::size_t>, std::vector<std::size_t>>
{
    PostingBuilder<DocId> document_builder(std::move(document_writer));
    PostingBuilder<Frequency> frequency_builder(std::move(frequency_writer));
    for (auto pos = first; pos != last; ++pos) {
        auto dseq = pos->docs;
        auto fseq = pos->freqs;
        for (auto doc : dseq) {
            document_builder.accumulate(doc);
        }
        for (auto freq : fseq) {
            frequency_builder.accumulate(freq);
        }
        document_builder.flush_segment(dout);
        frequency_builder.flush_segment(fout);
        *bar += 1;
    }
    return std::make_tuple(std::move(document_builder.offsets()),
                           std::move(frequency_builder.offsets()));
}

template <typename T>
void write_span(gsl::span<T> offsets, std::string const &file)
{
    std::ofstream os(file);
    auto bytes = gsl::as_bytes(offsets);
    os.write(reinterpret_cast<char const *>(bytes.data()), bytes.size());
}

inline void compress_binary_collection(std::string const &input,
                                       std::string_view fwd,
                                       std::string_view output,
                                       std::size_t const threads,
                                       Writer<DocId> document_writer,
                                       Writer<Frequency> frequency_writer)
{
    pisa::binary_freq_collection const collection(input.c_str());
    ProgressStatus status(collection.size(),
                          DefaultProgress("Compressing in parallel"),
                          std::chrono::milliseconds(100));
    tbb::task_group group;
    auto const num_terms = collection.size();
    std::vector<std::vector<std::size_t>> document_offsets(threads);
    std::vector<std::vector<std::size_t>> frequency_offsets(threads);
    std::vector<std::string> document_paths;
    std::vector<std::string> frequency_paths;
    std::vector<std::ofstream> document_streams;
    std::vector<std::ofstream> frequency_streams;
    auto for_each_batch = [threads](auto fn) {
        for (auto thread_idx = 0; thread_idx < threads; thread_idx += 1) {
            fn(thread_idx);
        }
    };
    for_each_batch([&](auto thread_idx) {
        auto document_batch =
            document_paths.emplace_back(fmt::format("{}.doc.batch.{}", output, thread_idx));
        auto frequency_batch =
            frequency_paths.emplace_back(fmt::format("{}.freq.batch.{}", output, thread_idx));
        document_streams.emplace_back(document_batch);
        frequency_streams.emplace_back(frequency_batch);
    });
    auto batch_size = num_terms / threads;
    for_each_batch([&](auto thread_idx) {
        group.run([thread_idx,
                   batch_size,
                   threads,
                   &collection,
                   &document_streams,
                   &frequency_streams,
                   &document_offsets,
                   &frequency_offsets,
                   &status,
                   &document_writer,
                   &frequency_writer]() {
            auto first = std::next(collection.begin(), thread_idx * batch_size);
            auto last = [&]() {
                if (thread_idx == threads - 1) {
                    return collection.end();
                }
                return std::next(collection.begin(), (thread_idx + 1) * batch_size);
            }();
            auto &dout = document_streams[thread_idx];
            auto &fout = frequency_streams[thread_idx];
            std::tie(document_offsets[thread_idx], frequency_offsets[thread_idx]) =
                compress_batch(first,
                               last,
                               dout,
                               fout,
                               document_writer,
                               frequency_writer,
                               tl::make_optional<ProgressStatus &>(status));
        });
    });
    group.wait();
    document_streams.clear();
    frequency_streams.clear();

    std::vector<std::size_t> all_document_offsets;
    std::vector<std::size_t> all_frequency_offsets;
    all_document_offsets.reserve(num_terms + 1);
    all_frequency_offsets.reserve(num_terms + 1);
    all_document_offsets.push_back(0);
    all_frequency_offsets.push_back(0);
    auto documents_file = fmt::format("{}.documents", output);
    auto frequencies_file = fmt::format("{}.frequencies", output);
    std::ofstream document_out(documents_file);
    std::ofstream frequency_out(frequencies_file);

    PostingBuilder<DocId>(document_writer).write_header(document_out);
    PostingBuilder<Frequency>(frequency_writer).write_header(frequency_out);

    {
        ProgressStatus merge_status(
            threads, DefaultProgress("Merging files"), std::chrono::milliseconds(500));
        for_each_batch([&](auto thread_idx) {
            std::transform(
                std::next(document_offsets[thread_idx].begin()),
                document_offsets[thread_idx].end(),
                std::back_inserter(all_document_offsets),
                [base = all_document_offsets.back()](auto offset) { return base + offset; });
            std::transform(
                std::next(frequency_offsets[thread_idx].begin()),
                frequency_offsets[thread_idx].end(),
                std::back_inserter(all_frequency_offsets),
                [base = all_frequency_offsets.back()](auto offset) { return base + offset; });
            std::ifstream docbatch(document_paths[thread_idx]);
            std::ifstream freqbatch(frequency_paths[thread_idx]);
            document_out << docbatch.rdbuf();
            frequency_out << freqbatch.rdbuf();
            merge_status += 1;
        });
    }

    std::cerr << "Writing offsets...";
    auto doc_offset_file = fmt::format("{}.document_offsets", output);
    auto freq_offset_file = fmt::format("{}.frequency_offsets", output);
    write_span(gsl::span<std::size_t const>(all_document_offsets), doc_offset_file);
    write_span(gsl::span<std::size_t const>(all_frequency_offsets), freq_offset_file);
    std::cerr << " Done.\n";

    std::cerr << "Writing sizes...";
    auto lengths = read_sizes(input);
    auto document_lengths_file = fmt::format("{}.document_lengths", output);
    write_span(gsl::span<std::uint32_t const>(lengths), document_lengths_file);
    float avg_len = calc_avg_length(gsl::span<std::uint32_t const>(lengths));
    std::cerr << " Done.\n";

    boost::property_tree::ptree pt;
    pt.put("documents.file", documents_file);
    pt.put("documents.offsets", doc_offset_file);
    pt.put("frequencies.file", frequencies_file);
    pt.put("frequencies.offsets", freq_offset_file);
    pt.put("stats.avg_document_length", avg_len);
    pt.put("stats.document_lengths", document_lengths_file);
    pt.put("lexicon.stemmer", "porter2"); // TODO(michal): Parametrize
    pt.put("lexicon.terms", fmt::format("{}.termlex", fwd));
    pt.put("lexicon.documents", fmt::format("{}.doclex", fwd));
    boost::property_tree::write_ini(fmt::format("{}.ini", output), pt);
}

auto verify_compressed_index(std::string const &input, std::string_view output)
    -> std::vector<std::string>;

} // namespace pisa::v1