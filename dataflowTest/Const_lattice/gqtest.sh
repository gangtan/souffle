#!/bin/bash



PROG=const_prop
echo "Job Started on $PBS_O_HOST at $(date)"

for Group in "Lines_400" #"Lines_50" "Lines_100" "Lines_200"
do
	echo "Group: $Group"
	for i in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19
	do
		printf "%d\t" $i
		outdir="./LatticeTestOutput/Scale/${Group}/$i"
		mkdir -p $outdir
		cmd="./../../src/souffle ${PROG}.dl --fact-dir=../LatticeTestFacts/Scale/${Group}/$i --output-dir=$outdir -p $outdir/${PROG}_profile.log"
		
		ts=$(date +%s%N)
		$cmd
		duration=$((($(date +%s%N) - $ts)/1000000))
		echo $duration

		sort -k1 $outdir/varEntry.csv > $outdir/sortedVarEntry.csv
		sort -k1 $outdir/varExit.csv > $outdir/sortedVarExit.csv
	done
done


echo "Job ended at $(date)"
