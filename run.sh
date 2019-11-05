cd ./build
home_dir="PATH_TO_HOME_DIR"
data_dir="../datasets/2018_coal/burner_"
nodes=2
number_threads=9
population=10
island=10
max_rec=10
offset_time=1
currentdate=$(date '+%m_%d_%H%M');


        # for population in 20
            #  do
                for check in 200
                  do
                    # for fold in {0..10}
                    for fold in 0
                      do
                        exp_name="$HOME/Dropbox/test_output/$currentdate/$population/$check/fold_$fold"
                        mkdir -p $exp_name
                       # file= $exp_name"/fitness_log.csv"

                        mpirun -np 9 ./mpi/examm_mpi --training_filenames ../datasets/2018_coal/burner_[0-9].csv  --test_filenames ../datasets/2018_coal/burner_1[0-1].csv  --time_offset $offset_time --input_parameter_names Conditioner_Inlet_Temp Conditioner_Outlet_Temp Coal_Feeder_Rate Primary_Air_Flow Primary_Air_Split System_Secondary_Air_Flow_Total Secondary_Air_Flow Secondary_Air_Split Tertiary_Air_Split Total_Comb_Air_Flow Supp_Fuel_Flow Main_Flm_Int --output_parameter_names Main_Flm_Int --population_size $population --max_genomes 10000 --bp_iterations 10  --output_directory $exp_name  --number_islands $island --possible_node_types simple UGRNN MGU GRU delta LSTM --num_genomes_check_on_island $check --check_on_island_method clear_island_with_worst_best_genome

                  done
                done
            #   done
