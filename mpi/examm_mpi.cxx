#include <chrono>

#include <iomanip>
using std::setw;
using std::fixed;
using std::setprecision;

#include <mutex>
using std::mutex;

#include <string>
using std::string;

#include <thread>
using std::thread;

#include <vector>
using std::vector;

#include "mpi.h"

#include "common/arguments.hxx"
#include "common/log.hxx"

#include "rnn/examm.hxx"

#include "time_series/time_series.hxx"

#define WORK_REQUEST_TAG 1
#define GENOME_LENGTH_TAG 2
#define GENOME_TAG 3
#define TERMINATE_TAG 4

mutex examm_mutex;

vector<string> arguments;

EXAMM *examm;

bool finished = false;

vector< vector< vector<double> > > training_inputs;
vector< vector< vector<double> > > training_outputs;
vector< vector< vector<double> > > validation_inputs;
vector< vector< vector<double> > > validation_outputs;

void send_work_request(int target) {
    int work_request_message[1];
    work_request_message[0] = 0;
    MPI_Send(work_request_message, 1, MPI_INT, target, WORK_REQUEST_TAG, MPI_COMM_WORLD);
}

void receive_work_request(int source) {
    MPI_Status status;
    int work_request_message[1];
    MPI_Recv(work_request_message, 1, MPI_INT, source, WORK_REQUEST_TAG, MPI_COMM_WORLD, &status);
}

RNN_Genome* receive_genome_from(int source) {
    MPI_Status status;
    int length_message[1];
    MPI_Recv(length_message, 1, MPI_INT, source, GENOME_LENGTH_TAG, MPI_COMM_WORLD, &status);

    int length = length_message[0];

    Log::debug("receiving genome of length: %d from: %d\n", length, source);

    char* genome_str = new char[length + 1];

    Log::debug("receiving genome from: %d\n", source);
    MPI_Recv(genome_str, length, MPI_CHAR, source, GENOME_TAG, MPI_COMM_WORLD, &status);

    genome_str[length] = '\0';

    Log::trace("genome_str:\n%s\n", genome_str);

    RNN_Genome* genome = new RNN_Genome(genome_str, length);

    delete [] genome_str;
    return genome;
}

void send_genome_to(int target, RNN_Genome* genome) {
    char *byte_array;
    int32_t length;

    genome->write_to_array(&byte_array, length);

    Log::debug("sending genome of length: %d to: %d\n", length, target);

    int length_message[1];
    length_message[0] = length;
    MPI_Send(length_message, 1, MPI_INT, target, GENOME_LENGTH_TAG, MPI_COMM_WORLD);

    Log::debug("sending genome to: %d\n", target);
    MPI_Send(byte_array, length, MPI_CHAR, target, GENOME_TAG, MPI_COMM_WORLD);
}

void send_terminate_message(int target) {
    int terminate_message[1];
    terminate_message[0] = 0;
    MPI_Send(terminate_message, 1, MPI_INT, target, TERMINATE_TAG, MPI_COMM_WORLD);
}

void receive_terminate_message(int source) {
    MPI_Status status;
    int terminate_message[1];
    MPI_Recv(terminate_message, 1, MPI_INT, source, TERMINATE_TAG, MPI_COMM_WORLD, &status);
}

void master(int max_rank) {
    //the "main" id will have already been set by the main function so we do not need to re-set it here
    Log::debug("MAX INT: %d\n", numeric_limits<int>::max());

    int terminates_sent = 0;

    while (true) {
        //wait for a incoming message
        MPI_Status status;
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        int source = status.MPI_SOURCE;
        int tag = status.MPI_TAG;
        Log::debug("probe returned message from: %d with tag: %d\n", source, tag);


        //if the message is a work request, send a genome

        if (tag == WORK_REQUEST_TAG) {
            receive_work_request(source);

            examm_mutex.lock();
            RNN_Genome *genome = examm->generate_genome();
            examm_mutex.unlock();

            if (genome == NULL) { //search was completed if it returns NULL for an individual
                //send terminate message
                Log::info("terminating worker: %d\n", source);
                send_terminate_message(source);
                terminates_sent++;

                Log::debug("sent: %d terminates of %d\n", terminates_sent, (max_rank - 1));
                if (terminates_sent >= max_rank - 1) return;

            } else {
                //genome->write_to_file( examm->get_output_directory() + "/before_send_gen_" + to_string(genome->get_generation_id()) );

                //send genome
                Log::debug("sending genome to: %d\n", source);
                send_genome_to(source, genome);

                //delete this genome as it will not be used again
                delete genome;
            }
        } else if (tag == GENOME_LENGTH_TAG) {
            Log::debug("received genome from: %d\n", source);
            RNN_Genome *genome = receive_genome_from(source);

            examm_mutex.lock();
            examm->insert_genome(genome);
            examm_mutex.unlock();

            //this genome will be deleted if/when removed from population
        } else {
            Log::fatal("ERROR: received message from %d with unknown tag: %d", source, tag);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
    }
}

void worker(int rank) {
    Log::set_id("worker_" + to_string(rank));

    while (true) {
        Log::debug("sending work request!\n");
        send_work_request(0);
        Log::debug("sent work request!\n");

        MPI_Status status;
        MPI_Probe(0, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        int tag = status.MPI_TAG;

        Log::debug("probe received message with tag: %d\n", tag);

        if (tag == TERMINATE_TAG) {
            Log::debug("received terminate tag!\n");
            receive_terminate_message(0);
            break;

        } else if (tag == GENOME_LENGTH_TAG) {
            Log::debug("received genome!\n");
            RNN_Genome* genome = receive_genome_from(0);

            //have each worker write the backproagation to a separate log file
            string log_id = "genome_" + to_string(genome->get_generation_id()) + "_worker_" + to_string(rank);
            Log::set_id(log_id);
            genome->backpropagate_stochastic(training_inputs, training_outputs, validation_inputs, validation_outputs);
            Log::release_id(log_id);

            //go back to the worker's log for MPI communication
            Log::set_id("worker_" + to_string(rank));

            send_genome_to(0, genome);

            delete genome;
        } else {
            Log::fatal("ERROR: received message with unknown tag: %d\n", tag);
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
    }

    //release the log file for the worker communication
    Log::release_id("worker_" + to_string(rank));
}

void get_individual_inputs(string str, vector<string>& tokens) {
   string word = "";
   for (auto x : str) {
       if (x == ',') {
           tokens.push_back(word);
           word = "";
       }else
           word = word + x;
   }
   tokens.push_back(word);
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);

    int rank, max_rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &max_rank);

    arguments = vector<string>(argv, argv + argc);

    Log::initialize(arguments);
    Log::set_rank(rank);
    Log::set_id("main_" + to_string(rank));
    Log::restrict_to_rank(0);

    TimeSeriesSets *time_series_sets = NULL;
    vector<string> inputs_removed_tokens  ;
    vector<string> outputs_removed_tokens ;

    if (rank == 0) {
        //only have the master process print TSS info
        time_series_sets = TimeSeriesSets::generate_from_arguments(arguments);

        if (argument_exists(arguments, "--write_time_series")) {
            string base_filename;
            get_argument(arguments, "--write_time_series", true, base_filename);
            time_series_sets->write_time_series_sets(base_filename);
        }
    } else {
        time_series_sets = TimeSeriesSets::generate_from_arguments(arguments);
    }

    int32_t time_offset = 1;
    get_argument(arguments, "--time_offset", true, time_offset);

    time_series_sets->export_training_series(time_offset, training_inputs, training_outputs);
    time_series_sets->export_test_series(time_offset, validation_inputs, validation_outputs);

    int number_inputs = time_series_sets->get_number_inputs();
    int number_outputs = time_series_sets->get_number_outputs();

    Log::debug("number_inputs: %d, number_outputs: %d\n", number_inputs, number_outputs);

    int32_t population_size;
    get_argument(arguments, "--population_size", true, population_size);

    int32_t number_islands;
    get_argument(arguments, "--number_islands", true, number_islands);

    int32_t max_genomes;
    get_argument(arguments, "--max_genomes", true, max_genomes);

    int32_t num_genomes_check_on_island;
    get_argument(arguments, "--num_genomes_check_on_island", false, num_genomes_check_on_island);

    string check_on_island_method = "";
    get_argument(arguments, "--check_on_island_method", false, check_on_island_method);

    int32_t bp_iterations;
    get_argument(arguments, "--bp_iterations", true, bp_iterations);

    double learning_rate = 0.001;
    get_argument(arguments, "--learning_rate", false, learning_rate);

    double high_threshold = 1.0;
    bool use_high_threshold = get_argument(arguments, "--high_threshold", false, high_threshold);

    double low_threshold = 0.05;
    bool use_low_threshold = get_argument(arguments, "--low_threshold", false, low_threshold);

    double dropout_probability = 0.0;
    bool use_dropout = get_argument(arguments, "--dropout_probability", false, dropout_probability);

    string output_directory = "";
    get_argument(arguments, "--output_directory", false, output_directory);

    vector<string> possible_node_types;
    get_argument_vector(arguments, "--possible_node_types", false, possible_node_types);

    int32_t rec_delay_min = 1;
    get_argument(arguments, "--rec_delay_min", false, rec_delay_min);

    int32_t rec_delay_max = 10;
    get_argument(arguments, "--rec_delay_max", false, rec_delay_max);

    string rec_sampling_population = "global";
    get_argument(arguments, "--rec_sampling_population", false, rec_sampling_population);

    string rec_sampling_distribution = "uniform";
    get_argument(arguments, "--rec_sampling_distribution", false, rec_sampling_distribution);

    string genome_file_name = "";
    get_argument(arguments, "--genome_bin", false, genome_file_name);
    int no_extra_inputs = 0 ;
    if (genome_file_name != "") {
        get_argument(arguments, "--extra_inputs", true, no_extra_inputs);
    }
    int no_extra_outputs = 0 ;
    if (genome_file_name != "") {
        get_argument(arguments, "--extra_outputs", false, no_extra_outputs);
    }
    string inputs_to_remove = "" ;
    if (genome_file_name != "") {
        get_argument(arguments, "--inputs_to_remove", false, inputs_to_remove);
    }

    string outputs_to_remove = "" ;
    if (genome_file_name != "") {
        get_argument(arguments, "--outputs_to_remove", false, outputs_to_remove);
    }
    int tl_version = 1 ;
    if (genome_file_name != "") {
        get_argument(arguments, "--tl_version", false, tl_version);
    }

    get_individual_inputs( inputs_to_remove, inputs_removed_tokens) ;
    get_individual_inputs( outputs_to_remove, outputs_removed_tokens) ;

    Log::clear_rank_restriction();

    if (rank == 0) {
        examm = new EXAMM(population_size, number_islands, max_genomes, num_genomes_check_on_island, check_on_island_method,
            time_series_sets->get_input_parameter_names(),
            time_series_sets->get_output_parameter_names(),
            time_series_sets->get_normalize_mins(),
            time_series_sets->get_normalize_maxs(),
            bp_iterations, learning_rate,
            use_high_threshold, high_threshold,
            use_low_threshold, low_threshold,
            use_dropout, dropout_probability,
            rec_delay_min, rec_delay_max,
            rec_sampling_population, rec_sampling_distribution,
            output_directory,
            genome_file_name,
            no_extra_inputs, no_extra_outputs,
            inputs_removed_tokens, outputs_removed_tokens,
            tl_version);

        if (possible_node_types.size() > 0) examm->set_possible_node_types(possible_node_types);

        master(max_rank);
    } else {
        worker(rank);
    }
    Log::set_id("main_" + to_string(rank));

    finished = true;

    Log::debug("rank %d completed!\n");
    Log::release_id("main_" + to_string(rank));

    MPI_Finalize();

    return 0;
}
