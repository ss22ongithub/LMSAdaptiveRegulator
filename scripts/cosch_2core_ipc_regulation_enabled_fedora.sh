#! /bin/bash
# Fedora / this-PC version of cosch_2core_ipc_regulation_enabled.sh.
# Changes: background core 3 -> 2 (cores 0-2 online, areg regulates 1 and 2);
# cset -> taskset (cgroup v2, no cset); SPEC run dir -> run_base_refrate_mytest;
# data under $HOME/Workspace/data owned by the invoking user; loop over
# benchmarks_all, skipping benchmarks that are not built; a background lbm runs
# from its own copy (.0001) when the foreground is lbm too.


#if SPEC Installation exist
export SPEC=${SPEC:-$HOME/spec2017}
export CPU_CORE_FOREGROUND=1
export CPU_CORE_BACKGROUND=2

export CPUSET_FORE_NAME="C1"
export CPUSET_BACK_NAME="C2"


export BASE_DATA_PATH=$HOME/Workspace/data/
export DATA_OWNER=$(id -un)
export RUN_DIR_NAME=run_base_refrate_mytest-m64.0000
export MEMG_PATH=without_regulation/ # default path


export benchmarks_single=(
"619.lbm_s"
)

export benchmarks_all=(
"500.perlbench_r"
"502.gcc_r"
"505.mcf_r"
"520.omnetpp_r"
"523.xalancbmk_r"
"525.x264_r"
"531.deepsjeng_r"
"541.leela_r"
"548.exchange2_r"
"557.xz_r"
"503.bwaves_r"
"507.cactuBSSN_r"
"508.namd_r"
"510.parest_r"
"511.povray_r"
"519.lbm_r"
"521.wrf_r"
"526.blender_r"
"527.cam4_r"
"538.imagick_r"
"544.nab_r"
"549.fotonik3d_r"
"554.roms_r"
"600.perlbench_s"
"602.gcc_s"
"605.mcf_s"
"620.omnetpp_s"
"623.xalancbmk_s"
"625.x264_s"
"631.deepsjeng_s"
"641.leela_s"
"648.exchange2_s"
"657.xz_s"
"603.bwaves_s"
"607.cactuBSSN_s"
"619.lbm_s"
"621.wrf_s"
"627.cam4_s"
"628.pop2_s"
"638.imagick_s"
"644.nab_s"
"649.fotonik3d_s"
"654.roms_s"
)


export benchmarks_intrate=(
"500.perlbench_r"
"502.gcc_r"
"505.mcf_r"
"520.omnetpp_r"
"523.xalancbmk_r"
"525.x264_r"
"531.deepsjeng_r"
"541.leela_r"
"548.exchange2_r"
"557.xz_r"
)

export benchmarks_intspeed=(
"600.perlbench_s"
"602.gcc_s"
"605.mcf_s"
"620.omnetpp_s"
"623.xalancbmk_s"
"625.x264_s"
"631.deepsjeng_s"
"641.leela_s"
"648.exchange2_s"
"657.xz_s"
)

export benchmarks_fprate=(
"503.bwaves_r"
"507.cactuBSSN_r"
"508.namd_r"
"510.parest_r"
"511.povray_r"
"519.lbm_r"
"521.wrf_r"
"526.blender_r"
"527.cam4_r"
"538.imagick_r"
"544.nab_r"
"549.fotonik3d_r"
"554.roms_r"
)

export benchmarks_fpspeed=(
"603.bwaves_s"
"607.cactuBSSN_s"
"619.lbm_s"
"621.wrf_s"
"627.cam4_s"
"628.pop2_s"
"638.imagick_s"
"644.nab_s"
"649.fotonik3d_s"
"654.roms_s"
)


function cleanup() {

#	LOG_FILEPATH=`ls -tr $SPEC/result/CPU2017*log | tail -1`
#	echo "Copying $LOG_FILEPATH to $BASE_DATA_PATH$MEMG_PATH$bm/"
#	cp "$LOG_FILEPATH" "$BASE_DATA_PATH$MEMG_PATH$bm/"

	echo "Stopping Perf($PERF_LLC_PID)"
	kill -9 $PERF_LLC_PID
}


# function exit() {
# 	echo "CLEANING UP ..."
# 	cleanup
# 	echo "EXITING..."
# 	exit
# }

 execute_config() {
    start_msg=$1
    cmd=$2
    end_msg="DONE"

    echo "$start_msg..."
    echo "sudo $cmd" | bash
    echo $end_msg
 }



 function preprocess_separate_load_store_misses(){
	if [ -n "$1" ]; then
    	FILEPATH=$1
    	grep "LLC-load-misses"	$FILEPATH > "$FILEPATH.lm"
    	grep "LLC-store-misses"	$FILEPATH > "$FILEPATH.sm"
 	fi
}

################# MAIN ##################################

function monitor_llc_cache_miss() {

# monitor the LLC cache misses
	PERF_LLC_FILEPATH=$1
	MONTOR_CORE=$2
	local INTERVAL_MSEC=1000

	perf stat -x, -e  LLC-load-misses,LLC-store-misses -C $MONTOR_CORE -I $INTERVAL_MSEC -o $PERF_LLC_FILEPATH &
	local PERF_LLC_PID=$!
	echo "PERF Recording LLC Misses $PERF_LLC_FILEPATH PID ($PERF_LLC_PID) on Core ($CPU_CORE_FOREGROUND)"
    sleep 1

    echo $PERF_LLC_PID
}

######## Step 1 : Ensure the setup is ready #############

#sudo bash -c isol_setup.sh

########Step 2: ###########################################
function run_benchmark() {
	dry_run=""
	export BENCHMARK_NAME=$1
	export rundt=$2
	local CPUSET_NAME=$3
	export COS=$4
	export PERF_TIMEOUT=10000

	local CPU_CORE=$CPU_CORE_FOREGROUND
	if [[ $CPUSET_NAME == $CPUSET_BACK_NAME ]] ; then
		CPU_CORE=$CPU_CORE_BACKGROUND
	fi


	export ITERATIONS=3

	export dt=`date +"%Y-%m-%d-%H-%M-%S"`

	export RUN_DATA_PATH="$BASE_DATA_PATH$MEMG_PATH$BENCHMARK_NAME/RUN$COS-$rundt"


	RUN_EXE_PATH="$SPEC/benchspec/CPU/$BENCHMARK_NAME/run/$RUN_DIR_NAME/"
	# lbm in the background and the foreground at once: give the background its own run dir
	if [[ $CPUSET_NAME == $CPUSET_BACK_NAME && $BENCHMARK_NAME == $FOREGROUND_TASK ]]; then
		RUN_EXE_PATH="$SPEC/benchspec/CPU/$BENCHMARK_NAME/run/${RUN_DIR_NAME%.0000}.0001/"
	fi
	echo "RUNEXE_PATH = $RUN_EXE_PATH"

	execute_config "Creating $RUN_DATA_PATH" "mkdir -p $RUN_DATA_PATH ; sudo chmod 755 $RUN_DATA_PATH; sudo chown $DATA_OWNER $RUN_DATA_PATH"

	# local PERF_LLC_PID =$(monitor_llc_cache_miss "$RUN_DATA_PATH/$BENCHMARK_NAME-llc$dry_run-$dt.csv" $CPU_CORE_FOREGROUND)

	#sudo bash -c "cd $SPEC; source shrc; cd -; \
	export PERF_IPC_FILEPATH="$RUN_DATA_PATH/$BENCHMARK_NAME-IPC$dry_run-$dt.txt"
	local UNIT="lmsar-bench-cpu$CPU_CORE-$$-$RANDOM"
	echo "$BENCHMARK_NAME started on Core($CPU_CORE) in scope $UNIT"
	echo "PERF Recording IPC in  $PERF_IPC_FILEPATH for $PERF_TIMEOUT ms"
	# cgroup v2 replacement for 'cset proc -s': user.slice is restricted to CPU 0,
	# so the benchmark runs in a transient scope allowed only on its own core.
	# perf stat starts the benchmark itself, so every child process is counted
	# from the first instruction, and stops it after PERF_TIMEOUT.
	sudo systemd-run --scope --quiet --unit=$UNIT --slice=lmsar_bench.slice -p AllowedCPUs=$CPU_CORE 		--uid=$(id -u) --gid=$(id -g) --setenv=SPEC=$SPEC -- 		perf stat -e instructions,cycles -o $PERF_IPC_FILEPATH --timeout $PERF_TIMEOUT 		taskset -c $CPU_CORE $SPEC/bin/specinvoke -i $ITERATIONS -d $RUN_EXE_PATH > $RUN_DATA_PATH/specinvoke.log 2>&1

	# cleanup: stop everything left in the scope (specinvoke's children)
	echo "Stopping $BENCHMARK_NAME (scope $UNIT) ..."
	sudo systemctl stop $UNIT.scope 2>/dev/null

	# preprocess_separate_load_store_misses "$PERF_LLC_FILEPATH"

}

################## Initialization function ##############################
function init(){
	execute_config "Switch off kernel buffer tracing! " "echo 0 >  /sys/kernel/tracing/tracing_on"

	if [ -d /sys/module/areg ]; then
	    export MEMG_PATH=with_ar/
	elif [ -d /sys/module/memguard ]; then
	    export MEMG_PATH=with_memguard/
	else
		 export MEMG_PATH=without_regulation/
	fi
	echo "$MEMG_PATH"

}

#####################################################################################r###################
rundt=`date +"%Y-%m-%d-%H-%M-%S"`
BACKGROUND_TASK="519.lbm_r"

#FOREGROUND_TASK="500.perlbench_r"
#run_benchmark $BACKGROUND_TASK $rundt $CPUSET_BACK_NAME "COSCHED" &
#pid_back=$!
#run_benchmark $FOREGROUND_TASK $rundt $CPUSET_FORE_NAME "COSCHED"
#wait $pid_back
#sleep 60

###### Run Initializations #################

init

#############################################  MAIN #########################################r###################

for benchmark in "${benchmarks_all[@]}";
do
	if [ ! -f "$SPEC/benchspec/CPU/$benchmark/run/$RUN_DIR_NAME/speccmds.cmd" ]; then
		echo "==== $benchmark SKIPPED: not built (no $RUN_DIR_NAME) ===="
		continue
	fi
	echo "====================================================$benchmark START============================================"
 	FOREGROUND_TASK=$benchmark
 	run_benchmark $BACKGROUND_TASK $rundt $CPUSET_BACK_NAME "COSCHED-$FOREGROUND_TASK"&
	pid_back=$!
	run_benchmark $FOREGROUND_TASK $rundt $CPUSET_FORE_NAME "COSCHED"

	wait $pid_back
	sleep 30
	echo "====================================================$benchmark ENDS ============================================"
done

##############################################################################################
