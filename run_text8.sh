make
if [ ! -e text8 ]; then
  wget http://mattmahoney.net/dc/text8.zip -O text8.gz
  gzip -d text8.gz -f
fi


gcc NNLM_ada_v1_acc.c -o NNLM_ada_v1 -lm -pthread -O3 -march=native -Wall -funroll-loops -Wno-unused-result

time ./NNLM_ada_v1 -train text8 -output _NNLM_ada_v1++.bin -cbow 1 -size 50 -window 2 -negative 5 -hs 0 -sample 1e-4 -threads 20 -binary 1 -iter 10

