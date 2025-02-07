#include "ModBaseCallerNode.h"

#include "modbase/remora_encoder.h"
#include "modbase/remora_utils.h"
#include "nn/ModBaseRunner.h"
#include "utils/base_mod_utils.h"
#include "utils/math_utils.h"
#include "utils/sequence_utils.h"
#include "utils/stats.h"
#include "utils/tensor_utils.h"

#include <nvtx3/nvtx3.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>
using namespace std::chrono_literals;

namespace dorado {

constexpr auto FORCE_TIMEOUT = 100ms;

ModBaseCallerNode::ModBaseCallerNode(std::vector<std::unique_ptr<ModBaseRunner>> model_runners,
                                     size_t remora_threads,
                                     size_t block_stride,
                                     size_t max_reads)
        : MessageSink(max_reads),
          m_runners(std::move(model_runners)),
          m_num_input_workers(remora_threads),
          m_block_stride(block_stride),
          m_batch_size(m_runners[0]->batch_size()),
          // TODO -- more principled calculation of output queue size
          m_processed_chunks(10 * max_reads) {
    init_modbase_info();
    for (int i = 0; i < m_runners[0]->num_callers(); i++) {
        m_chunk_queues.emplace_back(
                std::make_unique<utils::AsyncQueue<std::shared_ptr<RemoraChunk>>>(m_batch_size *
                                                                                  5));
    }

    // Spin up the processing threads:
    start_threads();
}

void ModBaseCallerNode::start_threads() {
    m_output_worker = std::make_unique<std::thread>(&ModBaseCallerNode::output_worker_thread, this);

    for (size_t worker_id = 0; worker_id < m_runners.size(); ++worker_id) {
        for (size_t model_id = 0; model_id < m_runners[worker_id]->num_callers(); ++model_id) {
            auto t = std::make_unique<std::thread>(&ModBaseCallerNode::modbasecall_worker_thread,
                                                   this, worker_id, model_id);
            m_runner_workers.push_back(std::move(t));
            ++m_num_active_runner_workers;
        }
    }
    for (size_t i = 0; i < m_num_input_workers; ++i) {
        auto t = std::make_unique<std::thread>(&ModBaseCallerNode::input_worker_thread, this);
        m_input_workers.push_back(std::move(t));
        ++m_num_active_input_workers;
    }
}

void ModBaseCallerNode::terminate_impl() {
    terminate_input_queue();
    for (auto& t : m_input_workers) {
        if (t->joinable()) {
            t->join();
        }
    }
    m_input_workers.clear();
    for (auto& t : m_runner_workers) {
        if (t->joinable()) {
            t->join();
        }
    }
    m_runner_workers.clear();
    if (m_output_worker && m_output_worker->joinable()) {
        m_output_worker->join();
    }
    m_output_worker.reset();
}

void ModBaseCallerNode::restart() {
    for (auto& runner : m_runners) {
        runner->restart();
    }
    for (auto& chunk_queue : m_chunk_queues) {
        chunk_queue->restart();
    }
    restart_input_queue();
    m_processed_chunks.restart();
    start_threads();
}

[[maybe_unused]] ModBaseCallerNode::Info ModBaseCallerNode::get_modbase_info_and_maybe_init(
        std::vector<std::reference_wrapper<ModBaseParams const>> const& base_mod_params,
        ModBaseCallerNode* node) {
    struct ModelInfo {
        std::vector<std::string> long_names;
        std::string alphabet;
        std::string motif;
        int motif_offset;
        size_t base_counts = 1;
    };

    std::string const allowed_bases = "ACGT";
    std::array<ModelInfo, 4> model_info;
    for (int b = 0; b < 4; ++b) {
        model_info[b].alphabet = allowed_bases[b];
    }

    for (const auto& params_ref : base_mod_params) {
        const auto& params = params_ref.get();
        auto base = params.motif[params.motif_offset];
        if (allowed_bases.find(base) == std::string::npos) {
            throw std::runtime_error("Invalid base in remora model metadata.");
        }
        auto& map_entry = model_info[RemoraUtils::BASE_IDS[base]];
        map_entry.long_names = params.mod_long_names;
        map_entry.alphabet += params.mod_bases;
        if (node) {
            map_entry.motif = params.motif;
            map_entry.motif_offset = params.motif_offset;
            map_entry.base_counts = params.base_mod_count + 1;
            node->m_num_states += params.base_mod_count;
        }
    }

    Info result;
    utils::BaseModContext context_handler;
    for (const auto& info : model_info) {
        for (const auto& name : info.long_names) {
            if (!result.long_names.empty())
                result.long_names += ' ';
            result.long_names += name;
        }
        result.alphabet += info.alphabet;
        if (node && !info.motif.empty()) {
            context_handler.set_context(info.motif, size_t(info.motif_offset));
        }
    }

    if (node) {
        node->m_base_mod_info = std::make_shared<BaseModInfo>(result.alphabet, result.long_names,
                                                              context_handler.encode());

        node->m_base_prob_offsets[0] = 0;
        node->m_base_prob_offsets[1] = model_info[0].base_counts;
        node->m_base_prob_offsets[2] = node->m_base_prob_offsets[1] + model_info[1].base_counts;
        node->m_base_prob_offsets[3] = node->m_base_prob_offsets[2] + model_info[2].base_counts;
    }

    return result;
}

void ModBaseCallerNode::init_modbase_info() {
    std::vector<std::reference_wrapper<ModBaseParams const>> base_mod_params;
    auto& runner = m_runners[0];
    for (size_t caller_id = 0; caller_id < runner->num_callers(); ++caller_id) {
        base_mod_params.emplace_back(runner->caller_params(caller_id));
    }
    get_modbase_info_and_maybe_init(base_mod_params, this);
}

void ModBaseCallerNode::input_worker_thread() {
    torch::InferenceMode inference_mode_guard;

    Message message;
    while (get_input_message(message)) {
        // If this message isn't a read, just forward it to the sink.
        if (!std::holds_alternative<std::shared_ptr<Read>>(message)) {
            send_message_to_sink(std::move(message));
            continue;
        }

        nvtx3::scoped_range range{"modbase_input_worker_thread"};
        // If this message isn't a read, we'll get a bad_variant_access exception.
        auto read = std::get<std::shared_ptr<Read>>(message);

        while (true) {
            stats::Timer timer;
            {
                nvtx3::scoped_range range{"base_mod_probs_init"};
                // initialize base_mod_probs _before_ we start handing out chunks
                read->base_mod_probs.resize(read->seq.size() * m_num_states, 0);
                for (size_t i = 0; i < read->seq.size(); ++i) {
                    // Initialize for what corresponds to 100% canonical base for each position.
                    int base_id = RemoraUtils::BASE_IDS[read->seq[i]];
                    if (base_id < 0) {
                        throw std::runtime_error("Invalid character in sequence.");
                    }
                    read->base_mod_probs[i * m_num_states + m_base_prob_offsets[base_id]] = 1.0f;
                }
            }
            read->base_mod_info = m_base_mod_info;

            std::vector<int> sequence_ints = utils::sequence_to_ints(read->seq);
            std::vector<uint64_t> seq_to_sig_map = utils::moves_to_map(
                    read->moves, m_block_stride, read->raw_data.size(0), read->seq.size() + 1);

            read->num_modbase_chunks = 0;
            read->num_modbase_chunks_called = 0;

            // all runners have the same set of callers, so we only need to use the first one
            auto& runner = m_runners[0];
            for (size_t caller_id = 0; caller_id < runner->num_callers(); ++caller_id) {
                nvtx3::scoped_range range{"generate_chunks"};
                auto& chunk_queue = m_chunk_queues.at(caller_id);

                // scale signal based on model parameters
                auto scaled_signal = runner->scale_signal(caller_id, read->raw_data, sequence_ints,
                                                          seq_to_sig_map);

                auto& params = runner->caller_params(caller_id);
                auto context_samples = (params.context_before + params.context_after);
                // One-hot encodes the kmer at each signal step for input into the network
                RemoraEncoder encoder(m_block_stride, context_samples, params.bases_before,
                                      params.bases_after);
                encoder.init(sequence_ints, seq_to_sig_map);

                auto context_hits = runner->get_motif_hits(caller_id, read->seq);
                m_num_context_hits += static_cast<int64_t>(context_hits.size());
                std::vector<std::shared_ptr<RemoraChunk>> reads_to_enqueue;
                reads_to_enqueue.reserve(context_hits.size());
                for (auto context_hit : context_hits) {
                    nvtx3::scoped_range range{"create_chunk"};
                    auto slice = encoder.get_context(context_hit);
                    // signal
                    auto input_signal = scaled_signal.index({torch::indexing::Slice(
                            slice.first_sample, slice.first_sample + slice.num_samples)});
                    if (slice.lead_samples_needed != 0 || slice.tail_samples_needed != 0) {
                        input_signal = torch::constant_pad_nd(input_signal,
                                                              {(int64_t)slice.lead_samples_needed,
                                                               (int64_t)slice.tail_samples_needed});
                    }
                    reads_to_enqueue.push_back(std::make_shared<RemoraChunk>(
                            read, input_signal, std::move(slice.data), context_hit));

                    ++read->num_modbase_chunks;
                }
                for (auto& chunk : reads_to_enqueue) {
                    chunk_queue->try_push(std::move(chunk));
                }
            }
            m_chunk_generation_ms += timer.GetElapsedMS();

            if (read->num_modbase_chunks != 0) {
                // Put the read in the working list
                std::scoped_lock<std::mutex> working_reads_lock(m_working_reads_mutex);
                m_working_reads.push_back(read);
            } else {
                // No modbases to call, pass directly to next node
                send_message_to_sink(read);
                ++m_num_non_mod_base_reads_pushed;
            }
            break;
        }
    }

    int num_remaining_workers = --m_num_active_input_workers;
    if (num_remaining_workers == 0) {
        for (auto& chunk_queue : m_chunk_queues) {
            chunk_queue->terminate();
        }
    }
}

void ModBaseCallerNode::modbasecall_worker_thread(size_t worker_id, size_t caller_id) {
    torch::InferenceMode inference_mode_guard;

    auto& runner = m_runners[worker_id];
    auto& chunk_queue = m_chunk_queues[caller_id];

    std::vector<std::shared_ptr<RemoraChunk>> batched_chunks;
    auto last_chunk_reserve_time = std::chrono::system_clock::now();

    size_t previous_chunk_count = 0;
    while (true) {
        nvtx3::scoped_range range{"modbasecall_worker_thread"};
        // Repeatedly attempt to complete the current batch with one acquisition of the
        // chunk queue mutex.
        auto grab_chunk = [&batched_chunks](std::shared_ptr<RemoraChunk>& chunk) {
            batched_chunks.push_back(std::move(chunk));
        };
        const auto status = chunk_queue->process_and_pop_n_with_timeout(
                grab_chunk, m_batch_size - batched_chunks.size(),
                last_chunk_reserve_time + FORCE_TIMEOUT);
        if (status == utils::AsyncQueueStatus::Terminate) {
            break;
        }

        // Reset timeout.
        last_chunk_reserve_time = std::chrono::system_clock::now();

        // We have just grabbed a number of chunks (0 in the case of timeout) from
        // the chunk queue and added them to batched_chunks.  Insert those chunks
        // into the model input tensors.
        assert(!batched_chunks.empty());
        for (size_t chunk_idx = previous_chunk_count; chunk_idx < batched_chunks.size();
             ++chunk_idx) {
            assert(chunk_idx < m_batch_size);
            const auto& chunk = batched_chunks[chunk_idx];
            runner->accept_chunk(caller_id, chunk_idx, chunk->signal, chunk->encoded_kmers);
        }

        // If we have a complete batch, or we have a partial batch and timed out,
        // then call what we have.
        if (batched_chunks.size() == m_batch_size ||
            (status == utils::AsyncQueueStatus::Timeout && !batched_chunks.empty())) {
            // Input tensor is full, let's get scores.
            call_current_batch(worker_id, caller_id, batched_chunks);
        }

        previous_chunk_count = batched_chunks.size();
    }

    // Basecall any remaining chunks.
    if (!batched_chunks.empty()) {
        call_current_batch(worker_id, caller_id, batched_chunks);
    }

    // Reduce the count of active model callers.  If this was the last active
    // model caller also send termination signal to sink
    int num_remaining_callers = --m_num_active_runner_workers;
    if (num_remaining_callers == 0) {
        m_processed_chunks.terminate();
    }
}

void ModBaseCallerNode::call_current_batch(
        size_t worker_id,
        size_t caller_id,
        std::vector<std::shared_ptr<RemoraChunk>>& batched_chunks) {
    nvtx3::scoped_range loop{"call_current_batch"};

    dorado::stats::Timer timer;
    auto results = m_runners[worker_id]->call_chunks(caller_id, batched_chunks.size());
    m_call_chunks_ms += timer.GetElapsedMS();

    // Convert results to float32 with one call and address via a raw pointer,
    // to avoid huge libtorch indexing overhead.
    auto results_f32 = results.to(torch::kFloat32);
    assert(results_f32.is_contiguous());
    const auto* const results_f32_ptr = results_f32.data_ptr<float>();

    auto row_size = results.size(1);

    // Put results into chunk
    for (size_t i = 0; i < batched_chunks.size(); ++i) {
        auto& chunk = batched_chunks[i];
        chunk->scores.resize(row_size);
        std::memcpy(chunk->scores.data(), &results_f32_ptr[i * row_size], row_size * sizeof(float));
        m_processed_chunks.try_push(std::move(chunk));
    }

    batched_chunks.clear();
    ++m_num_batches_called;
}

void ModBaseCallerNode::output_worker_thread() {
    torch::InferenceMode inference_mode_guard;

    // The m_processed_chunks lock is sufficiently contended that it's worth taking all
    // chunks available once we obtain it.
    std::vector<std::shared_ptr<RemoraChunk>> processed_chunks;
    auto grab_chunk = [&processed_chunks](std::shared_ptr<RemoraChunk>& chunk) {
        processed_chunks.push_back(std::move(chunk));
    };
    while (m_processed_chunks.process_and_pop_n(grab_chunk, m_processed_chunks.capacity()) ==
           utils::AsyncQueueStatus::Success) {
        nvtx3::scoped_range range{"modbase_output_worker_thread"};

        for (const auto& chunk : processed_chunks) {
            auto source_read = chunk->source_read.lock();
            int64_t result_pos = chunk->context_hit;
            int64_t offset =
                    m_base_prob_offsets[RemoraUtils::BASE_IDS[source_read->seq[result_pos]]];
            for (size_t i = 0; i < chunk->scores.size(); ++i) {
                source_read->base_mod_probs[m_num_states * result_pos + offset + i] =
                        static_cast<uint8_t>(std::min(std::floor(chunk->scores[i] * 256), 255.0f));
            }
            ++source_read->num_modbase_chunks_called;
        }
        processed_chunks.clear();

        // Now move any completed reads to the output queue
        std::vector<std::shared_ptr<Read>> completed_reads;
        std::unique_lock<std::mutex> working_reads_lock(m_working_reads_mutex);
        for (auto read_iter = m_working_reads.begin(); read_iter != m_working_reads.end();) {
            if ((*read_iter)->num_modbase_chunks_called.load() ==
                (*read_iter)->num_modbase_chunks) {
                completed_reads.push_back(*read_iter);
                read_iter = m_working_reads.erase(read_iter);
            } else {
                ++read_iter;
            }
        }
        working_reads_lock.unlock();
        for (auto& read : completed_reads) {
            send_message_to_sink(read);
            ++m_num_mod_base_reads_pushed;
        }
    }
}

std::unordered_map<std::string, double> ModBaseCallerNode::sample_stats() const {
    stats::NamedStats stats = stats::from_obj(m_work_queue);
    for (const auto& runner : m_runners) {
        const auto runner_stats = stats::from_obj(*runner);
        stats.insert(runner_stats.begin(), runner_stats.end());
    }
    stats["batches_called"] = m_num_batches_called;
    stats["partial_batches_called"] = m_num_partial_batches_called;
    stats["input_chunks_sleeps"] = m_num_input_chunks_sleeps;
    stats["call_chunks_ms"] = m_call_chunks_ms;
    stats["context_hits"] = m_num_context_hits;
    stats["mod_base_reads_pushed"] = m_num_mod_base_reads_pushed;
    stats["non_mod_base_reads_pushed"] = m_num_non_mod_base_reads_pushed;
    stats["chunk_generation_ms"] = m_chunk_generation_ms;
    return stats;
}

}  // namespace dorado
