//  Copyright 2013 Google Inc. All Rights Reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_STRING 100
#define EXP_TABLE_SIZE 1000
#define MAX_EXP 6
#define MAX_SENTENCE_LENGTH 1000
#define MAX_CODE_LENGTH 40

const int vocab_hash_size = 30000000;  // Maximum 30 * 0.7 = 21M words in the vocabulary

typedef float real;                    // Precision of float numbers

struct vocab_word {
	long long cn;
	int *point;
	char *word, *code, codelen;
};

char train_file[MAX_STRING], output_file[MAX_STRING];
char save_vocab_file[MAX_STRING], read_vocab_file[MAX_STRING];
struct vocab_word *vocab;
int binary = 0, cbow = 1, debug_mode = 2, window = 2, min_count = 5, num_threads = 24, min_reduce = 1;
int *vocab_hash;
//concat   layer1_size = feature_size*window*2
long long vocab_max_size = 1000, vocab_size = 0, feature_size=100,layer1_size = 100*2*2, layer2_size=50;
long long train_words = 0, word_count_actual = 0, iter = 5, file_size = 0, classes = 0;
real alpha = 0.1, starting_alpha, sample = 1e-4;
real *syn0, *syn2neg, *syn1neg,*expTable, *sigmoidTable, *tanhTable,*gradsq_syn0,*gradsq_syn1neg,*gradsq_syn2neg;
clock_t start;

int  negative = 5;
const int table_size = 1e8;
int *table;

void InitUnigramTable() {
	int a, i;
	double train_words_pow = 0;
	double d1, power = 0.75;
	table = (int *)malloc(table_size * sizeof(int));
	for (a = 0; a < vocab_size; a++) train_words_pow += pow(vocab[a].cn, power);
	i = 0;
	d1 = pow(vocab[i].cn, power) / train_words_pow;
	for (a = 0; a < table_size; a++) {
		table[a] = i;
		if (a / (double)table_size > d1) {
			i++;
			d1 += pow(vocab[i].cn, power) / train_words_pow;
		}
		if (i >= vocab_size) i = vocab_size - 1;
	}
}

// Reads a single word from a file, assuming space + tab + EOL to be word boundaries
void ReadWord(char *word, FILE *fin) {
	int a = 0, ch;
	while (!feof(fin)) {
		ch = fgetc(fin);
		if (ch == 13) continue;
		if ((ch == ' ') || (ch == '\t') || (ch == '\n')) {
			if (a > 0) {
				if (ch == '\n') ungetc(ch, fin);
				break;
			}
			if (ch == '\n') {
				strcpy(word, (char *)"</s>");
				return;
			} else continue;
		}
		word[a] = ch;
		a++;
		if (a >= MAX_STRING - 1) a--;   // Truncate too long words
	}
	word[a] = 0;
}

// Returns hash value of a word
int GetWordHash(char *word) {
	unsigned long long a, hash = 0;
	for (a = 0; a < strlen(word); a++) hash = hash * 257 + word[a];
	hash = hash % vocab_hash_size;
	return hash;
}

// Returns position of a word in the vocabulary; if the word is not found, returns -1
int SearchVocab(char *word) {
	unsigned int hash = GetWordHash(word);
	while (1) {
		if (vocab_hash[hash] == -1) return -1;
		if (!strcmp(word, vocab[vocab_hash[hash]].word)) return vocab_hash[hash];
		hash = (hash + 1) % vocab_hash_size;
	}
	return -1;
}

// Reads a word and returns its index in the vocabulary
int ReadWordIndex(FILE *fin) {
	char word[MAX_STRING];
	ReadWord(word, fin);
	if (feof(fin)) return -1;
	return SearchVocab(word);
}

// Adds a word to the vocabulary
int AddWordToVocab(char *word) {
	unsigned int hash, length = strlen(word) + 1;
	if (length > MAX_STRING) length = MAX_STRING;
	vocab[vocab_size].word = (char *)calloc(length, sizeof(char));
	strcpy(vocab[vocab_size].word, word);
	vocab[vocab_size].cn = 0;
	vocab_size++;
	// Reallocate memory if needed
	if (vocab_size + 2 >= vocab_max_size) {
		vocab_max_size += 1000;
		vocab = (struct vocab_word *)realloc(vocab, vocab_max_size * sizeof(struct vocab_word));
	}
	hash = GetWordHash(word);
	while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
	vocab_hash[hash] = vocab_size - 1;
	return vocab_size - 1;
}

// Used later for sorting by word counts
int VocabCompare(const void *a, const void *b) {
		return ((struct vocab_word *)b)->cn - ((struct vocab_word *)a)->cn;
}

// Sorts the vocabulary by frequency using word counts
void SortVocab() {
	int a, size;
	unsigned int hash;
	// Sort the vocabulary and keep </s> at the first position
	qsort(&vocab[1], vocab_size - 1, sizeof(struct vocab_word), VocabCompare);
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
	size = vocab_size;
	train_words = 0;
	for (a = 0; a < size; a++) {
		// Words occuring less than min_count times will be discarded from the vocab
		if ((vocab[a].cn < min_count) && (a != 0)) {
			vocab_size--;
			free(vocab[a].word);
		} else {
			// Hash will be re-computed, as after the sorting it is not actual
			hash=GetWordHash(vocab[a].word);
			while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
			vocab_hash[hash] = a;
			train_words += vocab[a].cn;
		}
	}
	vocab = (struct vocab_word *)realloc(vocab, (vocab_size + 1) * sizeof(struct vocab_word));
	// Allocate memory for the binary tree construction
	for (a = 0; a < vocab_size; a++) {
		vocab[a].code = (char *)calloc(MAX_CODE_LENGTH, sizeof(char));
		vocab[a].point = (int *)calloc(MAX_CODE_LENGTH, sizeof(int));
	}
}

// Reduces the vocabulary by removing infrequent tokens
void ReduceVocab() {
	int a, b = 0;
	unsigned int hash;
	for (a = 0; a < vocab_size; a++) if (vocab[a].cn > min_reduce) {
		vocab[b].cn = vocab[a].cn;
		vocab[b].word = vocab[a].word;
		b++;
	} else free(vocab[a].word);
	vocab_size = b;
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
	for (a = 0; a < vocab_size; a++) {
		// Hash will be re-computed, as it is not actual
		hash = GetWordHash(vocab[a].word);
		while (vocab_hash[hash] != -1) hash = (hash + 1) % vocab_hash_size;
		vocab_hash[hash] = a;
	}
	fflush(stdout);
	min_reduce++;
}



void LearnVocabFromTrainFile() {
	char word[MAX_STRING];
	FILE *fin;
	long long a, i;
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
	fin = fopen(train_file, "rb");
	if (fin == NULL) {
		printf("ERROR: training data file not found!\n");
		exit(1);
	}
	vocab_size = 0;
	AddWordToVocab((char *)"</s>");
	while (1) {
		ReadWord(word, fin);
		if (feof(fin)) break;
		train_words++;
		if ((debug_mode > 1) && (train_words % 100000 == 0)) {
			printf("%lldK%c", train_words / 1000, 13);
			fflush(stdout);
		}
		i = SearchVocab(word);
		if (i == -1) {
			a = AddWordToVocab(word);
			vocab[a].cn = 1;
		} else vocab[i].cn++;
		if (vocab_size > vocab_hash_size * 0.7) ReduceVocab();
	}
	SortVocab();
	if (debug_mode > 0) {
		printf("Vocab size: %lld\n", vocab_size);
		printf("Words in train file: %lld\n", train_words);

	}
	file_size = ftell(fin);
	fclose(fin);
}

void SaveVocab() {
	long long i;
	FILE *fo = fopen(save_vocab_file, "wb");
	for (i = 0; i < vocab_size; i++) fprintf(fo, "%s %lld\n", vocab[i].word, vocab[i].cn);
	fclose(fo);
}

void ReadVocab() {
	long long a, i = 0;
	char c;
	char word[MAX_STRING];
	FILE *fin = fopen(read_vocab_file, "rb");
	if (fin == NULL) {
		printf("Vocabulary file not found\n");
		exit(1);
	}
	for (a = 0; a < vocab_hash_size; a++) vocab_hash[a] = -1;
	vocab_size = 0;
	while (1) {
		ReadWord(word, fin);
		if (feof(fin)) break;
		a = AddWordToVocab(word);
		fscanf(fin, "%lld%c", &vocab[a].cn, &c);
		i++;
	}
	SortVocab();
	if (debug_mode > 0) {
		printf("Vocab size: %lld\n", vocab_size);
		printf("Words in train file: %lld\n", train_words);
	}
	fin = fopen(train_file, "rb");
	if (fin == NULL) {
		printf("ERROR: training data file not found!\n");
		exit(1);
	}
	fseek(fin, 0, SEEK_END);
	file_size = ftell(fin);
	fclose(fin);
}

void InitNet() {
	long long a, b;
	unsigned long long next_random = 1;
	a = posix_memalign((void **)&syn0, 128, (long long)vocab_size * feature_size * sizeof(real));
	a = posix_memalign((void **)&gradsq_syn0, 128, (long long)vocab_size * feature_size * sizeof(real));
	if (syn0 == NULL) {printf("Memory allocation failed\n"); exit(1);}



	if (negative>0) {
		//1layer-2layer  matrix with bias 
		a = posix_memalign((void **)&syn1neg, 128, (long long)layer1_size *layer2_size* sizeof(real));
		if (syn1neg == NULL) {printf("Memory allocation failed\n"); exit(1);}
		for (a = 0; a < layer2_size; a++) for (b = 0; b <= layer1_size; b++){
		next_random = next_random * (unsigned long long)25214903917 + 11;
		 syn1neg[a * layer1_size + b] = (((next_random & 0xFFFF) / (real)65536) - 0.5)/layer2_size;
		}

		//1layer-2layer  matrix with bias 
		a = posix_memalign((void **)&gradsq_syn1neg, 128, (long long)layer1_size *layer2_size* sizeof(real));
		if (gradsq_syn1neg == NULL) {printf("Memory allocation failed\n"); exit(1);}
		for (a = 0; a < layer2_size; a++) for (b = 0; b <= layer1_size; b++){
		next_random = next_random * (unsigned long long)25214903917 + 11;
		 gradsq_syn1neg[a * layer1_size + b] = 1e-8;
		}


		//2layer-output matrix  with bias 
		a = posix_memalign((void **)&syn2neg, 128, (long long)vocab_size * layer2_size * sizeof(real));
		if (syn2neg == NULL) {printf("Memory allocation failed\n"); exit(1);}
		for (a = 0; a < vocab_size; a++) for (b = 0; b <= layer2_size; b++)
		 syn2neg[a * layer2_size + b] = 0;


		//2layer-output matrix  with bias 
		a = posix_memalign((void **)&gradsq_syn2neg, 128, (long long)vocab_size * layer2_size * sizeof(real));
		if (gradsq_syn2neg == NULL) {printf("Memory allocation failed\n"); exit(1);}
		for (a = 0; a < vocab_size; a++) for (b = 0; b <= layer2_size; b++)
		 gradsq_syn2neg[a * layer2_size + b] = 1e-8;


	}



	//initialize syn0
	for (a = 0; a < vocab_size; a++) for (b = 0; b < feature_size; b++) {
		next_random = next_random * (unsigned long long)25214903917 + 11;
		syn0[a * feature_size + b] = (((next_random & 0xFFFF) / (real)65536) - 0.5)/layer1_size;
	}
	//initialize gradsq_syn0 to one 
	for (a = 0; a < vocab_size; a++) for (b = 0; b < feature_size; b++) {
		next_random = next_random * (unsigned long long)25214903917 + 11;
		gradsq_syn0[a * feature_size + b] = 1e-8;
	}

	
}

void *TrainModelThread(void *id) {
	long long a, b, d, cw, word, last_word, sentence_length = 0, sentence_position = 0;
	long long word_count = 0, last_word_count = 0, sen[MAX_SENTENCE_LENGTH + 1];
	long long l2, c, target, label, local_iter = iter;
	unsigned long long next_random = (long long)id;
	real f, g;
	clock_t now;
	real *neu1 = (real *)calloc(layer1_size+1, sizeof(real));
	real *neu1e = (real *)calloc(layer1_size+1, sizeof(real));
	real *neu2 = (real *)calloc(layer2_size+1, sizeof(real));
	real *neu2e = (real *)calloc(layer2_size+1, sizeof(real));
	FILE *fi = fopen(train_file, "rb");
	fseek(fi, file_size / (long long)num_threads * (long long)id, SEEK_SET);
	while (1){
		
		if (word_count - last_word_count > 10000) {
			word_count_actual += word_count - last_word_count;
			last_word_count = word_count;
			if ((debug_mode > 1)) {
				now=clock();
				printf("%cAlpha: %f  Progress: %.2f%%  Words/thread/sec: %.2fk  ", 13, alpha,
				 word_count_actual / (real)(iter * train_words + 1) * 100,
				 word_count_actual / ((real)(now - start + 1) / (real)CLOCKS_PER_SEC * 1000));
				fflush(stdout);
			}
			//alpha = starting_alpha * (1 - word_count_actual / (real)(iter * train_words + 1));
			//if (alpha < starting_alpha * 0.0001) alpha = starting_alpha * 0.0001;
		}
		if (sentence_length == 0) {
			while (1) {
				word = ReadWordIndex(fi);
				if (feof(fi)) break;
				if (word == -1) continue;
				word_count++;
				if (word == 0) break;
				// The subsampling randomly discards frequent words while keeping the ranking same
				if (sample > 0) {
					real ran = (sqrt(vocab[word].cn / (sample * train_words)) + 1) * (sample * train_words) / vocab[word].cn;
					next_random = next_random * (unsigned long long)25214903917 + 11;
					if (ran < (next_random & 0xFFFF) / (real)65536) continue;
				}
				sen[sentence_length] = word;
				sentence_length++;
				if (sentence_length >= MAX_SENTENCE_LENGTH) break;
			}
			sentence_position = 0;
		}
		if (feof(fi) || (word_count > train_words / num_threads)) {
			word_count_actual += word_count - last_word_count;
			local_iter--;
			if (local_iter == 0) break;
			word_count = 0;
			last_word_count = 0;
			sentence_length = 0;
			fseek(fi, file_size / (long long)num_threads * (long long)id, SEEK_SET);
			continue;
		}
		word = sen[sentence_position];
		if (word == -1) continue;
		for (c = 0; c < layer1_size; c++){
			neu1[c] = 0;
			neu1e[c] = 0;
		} 
		for (c = 0; c < layer2_size; c++){
			neu2[c] = 0;
			neu2e[c] = 0;
		} 
		
		//train the NNLM architecture
		//make neu1(concate)   : input to hidden1
		cw = 0;
		for (a = 0; a < window * 2 + 1; a++) if (a != window) {
			c = sentence_position - window+a ;
			if (c < 0) continue;
			if (c >= sentence_length) continue;
			last_word = sen[c];
			if (last_word == -1) continue;
			for (c = 0; c < feature_size; c++) neu1[c+cw*feature_size] += syn0[c + last_word * feature_size];
			cw++;
		}
		
		//make neu2 : hidden1 to hidden2 
		for (c = 0; c < layer2_size; c++) {
		           for (b = 0; b < layer1_size; b++) {
			
			
				neu2[c]+= neu1[b] * syn1neg[c*layer1_size + b]; //h2=w'*h1 (  c is  row,b is column )
			}
			
		}
		for (c = 0; c < layer2_size; c++) {
			if (neu2[c]> 6) neu2[c]=1.0;
			else if (neu2[c]< -6) neu2[c]=-1.0;
			// apply activation function
			else neu2[c]=tanhTable[(int)((neu2[c]+ MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))];
		}
		
		if (cw) {	

			// NEGATIVE SAMPLING
			if (negative > 0) for (d = 0; d < negative + 1; d++) {
				if (d == 0) {
					target = word;
					label = 1;
				} else {
					next_random = next_random * (unsigned long long)25214903917 + 11;
					target = table[(next_random >> 16) % table_size];
					if (target == 0) target = next_random % (vocab_size - 1) + 1;
					if (target == word) continue;
					label = 0;
				}
				l2 = target * layer2_size; // target word index 
				f = 0;
				//foward : hidden2 to output 
				for (c = 0; c < layer2_size; c++) f += neu2[c] * syn2neg[c + l2]; //O_i=w"*h2
				if (f > MAX_EXP) g = (label - 1) ;// g is the output  ( y_hat ) with '- 1'
				else if (f < -MAX_EXP) g = (label - 0) ;
				else g = (label - sigmoidTable[(int)((f + MAX_EXP) * (EXP_TABLE_SIZE / MAX_EXP / 2))]) ;

				//Back prop : Update v" & accumulate 
				for (c = 0; c < layer2_size; c++){
					
					//accumulate the error of layer2 to update syn1 later
					neu2e[c] += g * syn2neg[c + l2]*(1-neu2[c]*neu2[c]);
					
					gradsq_syn2neg[c + l2]+=g* neu2[c]*       g *neu2[c];
					syn2neg[c + l2] += alpha * g * neu2[c]    /sqrt(gradsq_syn2neg[c + l2]);
				} 




			}

			//update the neu1e
			for(b=0 ; b<layer1_size ; b++){
			    for(c=0 ; c<layer2_size ; c++){
				
					neu1e[b] += neu2e[c] * syn1neg[c*layer1_size+b];
				}
				
			}



			//update the syn1
			for(c=0 ; c<layer2_size ; c++){
				for(b=0 ; b<layer1_size ; b++){
					int p =c*layer1_size+b;
					gradsq_syn1neg[p]+=neu2e[c]*neu1[b]* neu2e[c]*neu1[b];
					syn1neg[p]+= alpha* neu2e[c]*neu1[b] /sqrt(gradsq_syn1neg[p]);
				}
			}
			
			b=0;
			// update the syn0 
			for (a = 0; a < window * 2 + 1 ; a++) if (a != window) {
				c = sentence_position - window + a;
				if (c < 0) continue;
				if (c >= sentence_length) continue;
				last_word = sen[c];
				if (last_word == -1) continue;
				for (c = 0; c < feature_size; c++){
					int p = c + last_word * feature_size;
					gradsq_syn0[p]+=neu1e[c+b*feature_size]* neu1e[c+b*feature_size];
					syn0[p] += alpha*   neu1e[c+b*feature_size]    /sqrt(gradsq_syn0[p]);
					
				} 
				b++;
			}
			
		}
			
		sentence_position++;
		if (sentence_position >= sentence_length) {
			sentence_length = 0;
			continue;
		}

		}
	
	fclose(fi);
	free(neu1);
	free(neu1e);
	free(neu2);
	free(neu2e);
	pthread_exit(NULL);
}

void TrainModel() {
	long a, b;
	FILE *fo;
	pthread_t *pt = (pthread_t *)malloc(num_threads * sizeof(pthread_t));
	printf("Starting training using file %s\n", train_file);
	starting_alpha = alpha;
	if (read_vocab_file[0] != 0) ReadVocab(); else LearnVocabFromTrainFile();

	if (save_vocab_file[0] != 0) SaveVocab();
	if (output_file[0] == 0) return;
			
	InitNet();
	if (negative > 0) InitUnigramTable();
	start = clock();
	for (a = 0; a < num_threads; a++) pthread_create(&pt[a], NULL, TrainModelThread, (void *)a);
	for (a = 0; a < num_threads; a++) pthread_join(pt[a], NULL);
	fo = fopen(output_file, "wb");

	// Save the word vectors
	fprintf(fo, "%lld %lld\n", vocab_size, feature_size);
	for (a = 0; a < vocab_size; a++) {
		fprintf(fo, "%s ", vocab[a].word);
		if (binary) for (b = 0; b < feature_size; b++) fwrite(&syn0[a * feature_size + b], sizeof(real), 1, fo);
		else for (b = 0; b < feature_size; b++) fprintf(fo, "%lf ", syn0[a * feature_size + b]);
		fprintf(fo, "\n");
	}

	fclose(fo);
}

int ArgPos(char *str, int argc, char **argv) {
	int a;
	for (a = 1; a < argc; a++) if (!strcmp(str, argv[a])) {
		if (a == argc - 1) {
			printf("Argument missing for %s\n", str);
			exit(1);
		}
		return a;
	}
	return -1;
}

int main(int argc, char **argv) {
	int i;
	if (argc == 1) {
		printf("WORD VECTOR estimation toolkit v 0.1c\n\n");
		printf("Options:\n");
		printf("Parameters for training:\n");
		printf("\t-train <file>\n");
		printf("\t\tUse text data from <file> to train the model\n");
		printf("\t-output <file>\n");
		printf("\t\tUse <file> to save the resulting word vectors / word clusters\n");
		printf("\t-size <int>\n");
		printf("\t\tSet size of word vectors; default is 100\n");
		printf("\t-window <int>\n");
		printf("\t\tSet max skip length between words; default is 5\n");
		printf("\t-sample <float>\n");
		printf("\t\tSet threshold for occurrence of words. Those that appear with higher frequency in the training data\n");
		printf("\t\twill be randomly down-sampled; default is 1e-3, useful range is (0, 1e-5)\n");
		printf("\t-layer1_size <int>\n");
		printf("\t\thiddenlayer size * window * 2\n");
		printf("\t-negative <int>\n");
		printf("\t\tNumber of negative examples; default is 5, common values are 3 - 10 (0 = not used)\n");
		printf("\t-threads <int>\n");
		printf("\t\tUse <int> threads (default 12)\n");
		printf("\t-iter <int>\n");
		printf("\t\tRun more training iterations (default 5)\n");
		printf("\t-min-count <int>\n");
		printf("\t\tThis will discard words that appear less than <int> times; default is 5\n");
		printf("\t-alpha <float>\n");
		printf("\t\tSet the starting learning rate; default is 0.025 for skip-gram and 0.05 for CBOW\n");
		printf("\t-classes <int>\n");
		printf("\t\tOutput word classes rather than word vectors; default number of classes is 0 (vectors are written)\n");
		printf("\t-debug <int>\n");
		printf("\t\tSet the debug mode (default = 2 = more info during training)\n");
		printf("\t-binary <int>\n");
		printf("\t\tSave the resulting vectors in binary moded; default is 0 (off)\n");
		printf("\t-save-vocab <file>\n");
		printf("\t\tThe vocabulary will be saved to <file>\n");
		printf("\t-read-vocab <file>\n");
		printf("\t\tThe vocabulary will be read from <file>, not constructed from the training data\n");
		printf("\t-cbow <int>\n");
		printf("\t\tUse the continuous bag of words model; default is 1 (use 0 for skip-gram model)\n");
		printf("\nExamples:\n");
		printf("./order -train data.txt -output vec.txt -feature_size 200 -window 2 -sample 1e-4 -negative 5 -layer1_size 200*2*2 -binary 0 -cbow 1 -iter 3\n\n");
		return 0;
	}
	output_file[0] = 0;
	save_vocab_file[0] = 0;
	read_vocab_file[0] = 0;
	if ((i = ArgPos((char *)"-size", argc, argv)) > 0) feature_size = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-train", argc, argv)) > 0) strcpy(train_file, argv[i + 1]);
	if ((i = ArgPos((char *)"-save-vocab", argc, argv)) > 0) strcpy(save_vocab_file, argv[i + 1]);
	if ((i = ArgPos((char *)"-read-vocab", argc, argv)) > 0) strcpy(read_vocab_file, argv[i + 1]);
	if ((i = ArgPos((char *)"-debug", argc, argv)) > 0) debug_mode = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-binary", argc, argv)) > 0) binary = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-cbow", argc, argv)) > 0) cbow = atoi(argv[i + 1]);
	if (cbow) alpha = 0.1;
	if ((i = ArgPos((char *)"-alpha", argc, argv)) > 0) alpha = atof(argv[i + 1]);
	if ((i = ArgPos((char *)"-output", argc, argv)) > 0) strcpy(output_file, argv[i + 1]);
	if ((i = ArgPos((char *)"-window", argc, argv)) > 0) window = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-sample", argc, argv)) > 0) sample = atof(argv[i + 1]);
	if ((i = ArgPos((char *)"-layer1_size", argc, argv)) > 0) layer1_size = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-negative", argc, argv)) > 0) negative = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-threads", argc, argv)) > 0) num_threads = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-iter", argc, argv)) > 0) iter = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-min-count", argc, argv)) > 0) min_count = atoi(argv[i + 1]);
	if ((i = ArgPos((char *)"-classes", argc, argv)) > 0) classes = atoi(argv[i + 1]);
           layer1_size = feature_size * window * 2;
           layer2_size = feature_size;
           vocab = (struct vocab_word *)calloc(vocab_max_size, sizeof(struct vocab_word));
	vocab_hash = (int *)calloc(vocab_hash_size, sizeof(int));
	expTable = (real *)malloc((EXP_TABLE_SIZE + 1) * sizeof(real));
	sigmoidTable = (real *)malloc((EXP_TABLE_SIZE + 1) * sizeof(real));
	tanhTable = (real *)malloc((EXP_TABLE_SIZE + 1) * sizeof(real));
	for (i = 0; i < EXP_TABLE_SIZE; i++) {
		expTable[i] = exp((i / (real)EXP_TABLE_SIZE * 2 - 1) * MAX_EXP); // Precompute the exp() table
		sigmoidTable[i] = expTable[i] / (expTable[i] + 1);                   // Precompute f(x) = x / (x + 1)
		//printf("%f \n",sigmoidTable[i]);
		
	}

	for (i = 0; i < EXP_TABLE_SIZE; i++) {

		tanhTable[i] = (expTable[i]-expTable[EXP_TABLE_SIZE-1-i]) / (expTable[i] + expTable[EXP_TABLE_SIZE-1-i]);  

		
	}
	TrainModel();
	return 0;
}
