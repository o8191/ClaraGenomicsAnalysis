/*
* Copyright (c) 2019, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include <file_location.hpp>
#include <claraparabricks/genomeworks/cudapoa/cudapoa.hpp>
#include <claraparabricks/genomeworks/cudapoa/batch.hpp>
#include <claraparabricks/genomeworks/utils/signed_integer_utils.hpp>
#include <claraparabricks/genomeworks/utils/cudautils.hpp>
#include <claraparabricks/genomeworks/cudapoa/utils.hpp> // for get_multi_batch_sizes()

#include <cuda_runtime_api.h>
#include <vector>
#include <string>
#include <unistd.h>

using namespace claraparabricks::genomeworks;
using namespace claraparabricks::genomeworks::cudapoa;

std::unique_ptr<Batch> initialize_batch(bool msa, bool banded_alignment, const BatchSize& batch_size)
{
    // Get device information.
    int32_t device_count = 0;
    CGA_CU_CHECK_ERR(cudaGetDeviceCount(&device_count));
    assert(device_count > 0);

    size_t total = 0, free = 0;
    cudaSetDevice(0); // Using first GPU for sample.
    cudaMemGetInfo(&free, &total);

    // Initialize internal logging framework.
    Init();

    // Initialize CUDAPOA batch object for batched processing of POAs on the GPU.
    const int32_t device_id      = 0;
    cudaStream_t stream          = 0;
    size_t mem_per_batch         = 0.9 * free; // Using 90% of GPU available memory for CUDAPOA batch.
    const int32_t mismatch_score = -6, gap_score = -8, match_score = 8;

    std::unique_ptr<Batch> batch = create_batch(device_id,
                                                stream,
                                                mem_per_batch,
                                                msa ? OutputType::msa : OutputType::consensus,
                                                batch_size,
                                                gap_score,
                                                mismatch_score,
                                                match_score,
                                                banded_alignment);

    return std::move(batch);
}

void process_batch(Batch* batch, bool msa, bool print)
{
    batch->generate_poa();

    StatusType status = StatusType::success;
    if (msa)
    {
        // Grab MSA results for all POA groups in batch.
        std::vector<std::vector<std::string>> msa; // MSA per group
        std::vector<StatusType> output_status;     // Status of MSA generation per group

        status = batch->get_msa(msa, output_status);
        if (status != StatusType::success)
        {
            std::cerr << "Could not generate MSA for batch : " << status << std::endl;
        }

        for (int32_t g = 0; g < get_size(msa); g++)
        {
            if (output_status[g] != StatusType::success)
            {
                std::cerr << "Error generating  MSA for POA group " << g << ". Error type " << output_status[g] << std::endl;
            }
            else
            {
                if (print)
                {
                    for (const auto& alignment : msa[g])
                    {
                        std::cout << alignment << std::endl;
                    }
                }
            }
        }
    }
    else
    {
        // Grab consensus results for all POA groups in batch.
        std::vector<std::string> consensus;          // Consensus string for each POA group
        std::vector<std::vector<uint16_t>> coverage; // Per base coverage for each consensus
        std::vector<StatusType> output_status;       // Status of consensus generation per group

        status = batch->get_consensus(consensus, coverage, output_status);
        if (status != StatusType::success)
        {
            std::cerr << "Could not generate consensus for batch : " << status << std::endl;
        }

        for (int32_t g = 0; g < get_size(consensus); g++)
        {
            if (output_status[g] != StatusType::success)
            {
                std::cerr << "Error generating consensus for POA group " << g << ". Error type " << output_status[g] << std::endl;
            }
            else
            {
                if (print)
                {
                    std::cout << consensus[g] << std::endl;
                }
            }
        }
    }
}

int main(int argc, char** argv)
{
    // Process options
    int c            = 0;
    bool msa         = false;
    bool long_read   = false;
    bool banded      = true;
    bool help        = false;
    bool print       = false;
    bool print_graph = false;

    while ((c = getopt(argc, argv, "mlfpgh")) != -1)
    {
        switch (c)
        {
        case 'm':
            msa = true;
            break;
        case 'l':
            long_read = true;
            break;
        case 'f':
            banded = false;
            break;
        case 'p':
            print = true;
            break;
        case 'g':
            print_graph = true;
            break;
        case 'h':
            help = true;
            break;
        }
    }

    if (help)
    {
        std::cout << "CUDAPOA API sample program. Runs consensus or MSA generation on pre-canned data." << std::endl;
        std::cout << "Usage:" << std::endl;
        std::cout << "./sample_cudapoa [-m] [-h]" << std::endl;
        std::cout << "-m : Generate MSA (if not provided, generates consensus by default)" << std::endl;
        std::cout << "-l : Perform long-read sample (if not provided, will run short-read sample by default)" << std::endl;
        std::cout << "-f : Perform full alignment (if not provided, banded alignment is used by default)" << std::endl;
        std::cout << "-p : Print the MSA or consensus output to stdout" << std::endl;
        std::cout << "-g : Print POA graph in dot format, this option is only for long-read sample" << std::endl;
        std::cout << "-h : Print help message" << std::endl;
        std::exit(0);
    }

    // Load input data. Each window is represented as a vector of strings. The sample
    // data has many such windows to process, hence the data is loaded into a vector
    // of vector of strings.
    std::vector<std::vector<std::string>> windows;

    if (long_read)
    {
        const std::string input_file = std::string(CUDAPOA_BENCHMARK_DATA_DIR) + "/sample-bonito.txt";
        parse_window_data_file(windows, input_file, -1);
    }
    else
    {
        const std::string input_file = std::string(CUDAPOA_BENCHMARK_DATA_DIR) + "/sample-windows.txt";
        parse_window_data_file(windows, input_file, 1000);
    }

    // Create a vector of POA groups based on windows
    std::vector<Group> poa_groups(windows.size());
    for (int32_t i = 0; i < get_size(windows); ++i)
    {
        Group& group = poa_groups[i];
        // Create a new entry for each sequence and add to the group.
        for (const auto& seq : windows[i])
        {
            Entry poa_entry{};
            poa_entry.seq     = seq.c_str();
            poa_entry.length  = seq.length();
            poa_entry.weights = nullptr;
            group.push_back(poa_entry);
        }
    }

    // analyze the POA groups and create a minimal set of batches to process them all
    std::vector<BatchSize> list_of_batch_sizes;
    std::vector<std::vector<int32_t>> list_of_groups_per_batch;

    get_multi_batch_sizes(list_of_batch_sizes, list_of_groups_per_batch, poa_groups, banded, msa);

    int32_t group_count_offset = 0;

    for (int32_t b = 0; b < get_size(list_of_batch_sizes); b++)
    {
        auto& batch_size      = list_of_batch_sizes[b];
        auto& batch_group_ids = list_of_groups_per_batch[b];

        // Initialize batch.
        std::unique_ptr<Batch> batch = initialize_batch(msa, banded, batch_size);

        // Loop over all the POA groups for the current batch, add them to the batch and process them.
        int32_t group_count = 0;

        for (int32_t i = 0; i < get_size(batch_group_ids);)
        {
            Group& group = poa_groups[batch_group_ids[i]];
            std::vector<StatusType> seq_status;
            StatusType status = batch->add_poa_group(seq_status, group);

            // NOTE: If number of batch groups smaller than batch capacity, then run POA generation
            // once last POA group is added to batch.
            if (status == StatusType::exceeded_maximum_poas || (i == get_size(batch_group_ids) - 1))
            {
                // at least one POA should have been added before processing the batch
                if (batch->get_total_poas() > 0)
                {
                    // No more POA groups can be added to batch. Now process batch.
                    process_batch(batch.get(), msa, print);

                    if (print_graph && long_read)
                    {
                        std::vector<DirectedGraph> graph;
                        std::vector<StatusType> graph_status;
                        batch->get_graphs(graph, graph_status);
                        for (auto& g : graph)
                        {
                            std::cout << g.serialize_to_dot() << std::endl;
                        }
                    }

                    // After MSA/consensus is generated for batch, reset batch to make room for next set of POA groups.
                    batch->reset();

                    // In case that number of batch groups is more than the capacity available on GPU, the for loop breaks into smaller number of groups.
                    // if adding group i in batch->add_poa_group is not successful, it wont be processed in this iteration, therefore we print i-1
                    // to account for the fact that group i was excluded at this round.
                    if (status == StatusType::success)
                    {
                        std::cout << "Processed groups " << group_count + group_count_offset << " - " << i + group_count_offset << " (batch " << b << ")" << std::endl;
                    }
                    else
                    {
                        std::cout << "Processed groups " << group_count + group_count_offset << " - " << i - 1 + group_count_offset << " (batch " << b << ")" << std::endl;
                    }
                }
                else
                {
                    // the POA was too large to be added to the GPU, skip and move on
                    std::cout << "Could not add POA group " << batch_group_ids[i] << " to batch " << b << std::endl;
                    i++;
                }

                group_count = i;
            }

            if (status == StatusType::success)
            {
                // Check if all sequences in POA group wre added successfully.
                for (const auto& s : seq_status)
                {
                    if (s == StatusType::exceeded_maximum_sequence_size)
                    {
                        std::cerr << "Dropping sequence because sequence exceeded maximum size" << std::endl;
                    }
                }
                i++;
            }

            if (status != StatusType::exceeded_maximum_poas && status != StatusType::success)
            {
                std::cout << "Could not add POA group " << batch_group_ids[i] << " to batch " << b << ". Error code " << status << std::endl;
                i++;
            }
        }

        group_count_offset += get_size(batch_group_ids);
    }

    return 0;
}
