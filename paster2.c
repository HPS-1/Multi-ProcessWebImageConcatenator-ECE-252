#define _DEFAULT_SOURCE
#include <curl/curl.h>

#include "paster2.h"
//#include "buffer.h"
#include <semaphore.h>
#include <stdbool.h>
#include <sys/shm.h>
#include <sys/stat.h>

#include "crc.h"
#include "zutil.h"
#include "arpa/inet.h"

unsigned int* total_frags_produced = NULL;
unsigned int* total_frags_consumed = NULL;
unsigned int* server_num = NULL;
unsigned char* filtered_buffer = NULL;
sem_t* spaces = NULL; //semaphore indicating available spaces in the buffer
sem_t* items = NULL; //semaphore indicating available items in the buffer
sem_t* mut = NULL; //semaphore used as mutex lock to lock/unlock the buffer. NOTE: only one process could use the buffer at a time
sem_t* mut_pro = NULL; //semaphore used as mutex lock to lock/unlock total_frags_produced
sem_t* mut_con = NULL; //semaphore used as mutex lock to lock/unlock total_frags_consumed

void option_parser(char *argv[6], int * b_s, int * p_c, int * c_c, int * s_t, int * i_n) {
    *b_s = atoi(argv[1]);
    *p_c = atoi(argv[2]);
    *c_c = atoi(argv[3]);
    *s_t = atoi(argv[4]);
    *i_n = atoi(argv[5]);
}

#define ECE252_HEADER "X-Ece252-Fragment: "
#define BUF_SIZE 1048576  /* 1024*1024 = 1M */
#define BUF_INC  524288   /* 1024*512  = 0.5M */
#define max(a, b) \
   ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

typedef struct recv_buf2 {
	char* buf;       /* memory to hold a copy of received data */
	size_t size;     /* size of valid data in buf in bytes*/
	size_t max_size; /* max capacity of buf in bytes*/
	int seq;         /* >=0 sequence number extracted from http header */
					 /* <0 indicates an invalid seq number */
} RECV_BUF;

typedef struct recv_buf3 {
	char buf[BUF_SIZE];       /* memory to hold a copy of received data */
	size_t size;     /* size of valid data in buf in bytes*/
	size_t max_size; /* max capacity of buf in bytes*/
	int seq;         /* >=0 sequence number extracted from http header */
					 /* <0 indicates an invalid seq number */
} RECV_BUF_FIXED;

RECV_BUF_FIXED* buffer_p2c = NULL;
size_t* producer_index = 0;
size_t* consumer_index = 0;

//the four functions below are copied from lab2
//------------copy start-----------------
size_t header_cb_curl(char* p_recv, size_t size, size_t nmemb, void* userdata)
{
	int realsize = size * nmemb;
	RECV_BUF* p = userdata;


	if (realsize > strlen(ECE252_HEADER) &&
		strncmp(p_recv, ECE252_HEADER, strlen(ECE252_HEADER)) == 0) {

		/* extract img sequence number */
		p->seq = atoi(p_recv + strlen(ECE252_HEADER));

	}
	return realsize;
}

size_t write_cb_curl3(char* p_recv, size_t size, size_t nmemb, void* p_userdata)
{
	size_t realsize = size * nmemb;
	RECV_BUF* p = (RECV_BUF*)p_userdata;

	if (p->size + realsize + 1 > p->max_size) {/* hope this rarely happens */
		/* received data is not 0 terminated, add one byte for terminating 0 */
		size_t new_size = p->max_size + max(BUF_INC, realsize + 1);
		char* q = realloc(p->buf, new_size);
		if (q == NULL) {
			perror("realloc"); /* out of memory */
			return -1;
		}
		p->buf = q;
		p->max_size = new_size;
	}

	memcpy(p->buf + p->size, p_recv, realsize); /*copy data from libcurl*/
	p->size += realsize;
	p->buf[p->size] = 0;

	return realsize;
}


int recv_buf_init(RECV_BUF* ptr, size_t max_size)
{
	void* p = NULL;

	if (ptr == NULL) {
		return 1;
	}

	p = malloc(max_size);
	if (p == NULL) {
		return 2;
	}

	ptr->buf = p;
	ptr->size = 0;
	ptr->max_size = max_size;
	ptr->seq = -1;              /* valid seq should be non-negative */
	return 0;
}

int recv_buf_cleanup(RECV_BUF* ptr)
{
	if (ptr == NULL) {
		return 1;
	}

	free(ptr->buf);
	ptr->size = 0;
	ptr->max_size = 0;
	return 0;
}
//------------copy end-----------------

//the callback() function below is actually the producer routine
void* callback(char* args, sem_t* mut, size_t* index_ptr, int buf_size) {

	char* url = args;

	RECV_BUF recv_buf;
	recv_buf_init(&recv_buf, BUF_SIZE);

	CURL* curl_handle;
	CURLcode res;
	//printf("^^^^^^^^^^^^^^^^^^^^^^^^^^^the REAL URL is %s+++\n", url);
	curl_global_init(CURL_GLOBAL_DEFAULT);
	curl_handle = curl_easy_init();
	curl_easy_setopt(curl_handle, CURLOPT_URL, url);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_cb_curl3);
	curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void*)&recv_buf);
	curl_easy_setopt(curl_handle, CURLOPT_HEADERFUNCTION, header_cb_curl);
	curl_easy_setopt(curl_handle, CURLOPT_HEADERDATA, (void*)&recv_buf);
	curl_easy_setopt(curl_handle, CURLOPT_USERAGENT, "libcurl-agent/1.0");
	res = curl_easy_perform(curl_handle);

	sem_wait(mut);
	buffer_p2c[(*index_ptr)].seq = recv_buf.seq;
	//printf("$$$Here we are inside callback()$$$\nThe seq num in recv_buf.seq (i.e. temporary buffer) is:%d\nThe seq num in buffer_p2c[index].seq (i.e. p2c buffer) is:%d\nAnd the index is:%lu\n",
		//recv_buf.seq, buffer_p2c[index].seq, index);
	memset(buffer_p2c[(*index_ptr)].buf, 0, (size_t)(BUF_SIZE*sizeof(char)));//clear what's in the p2c buffer now
	memcpy(buffer_p2c[(*index_ptr)].buf, recv_buf.buf, recv_buf.size);
	buffer_p2c[(*index_ptr)].size = recv_buf.size;
	//printf("$$$Here we are inside callback()$$$\nThe size in recv_buf.seq (i.e. temporary buffer) is:%lu\nThe size in buffer_p2c[index].size (i.e. p2c buffer) is:%lu\n",
		//recv_buf.size, buffer_p2c[index].size);
	buffer_p2c[(*index_ptr)].max_size = recv_buf.max_size;
	//printf("$$$Here we are inside callback()$$$\nThe max_size in recv_buf.max_size (i.e. temporary buffer) is:%lu\nThe max_size in buffer_p2c[index].max_size (i.e. p2c buffer) is:%lu\n",
		//recv_buf.max_size, buffer_p2c[index].max_size);
	(*index_ptr) = ((*index_ptr) + 1) % buf_size;
	

	recv_buf_cleanup(&recv_buf);
	curl_easy_cleanup(curl_handle);
	curl_global_cleanup();
	sem_post(mut);

	return NULL;
}

//the callback2() function below is actually the consumer routine
void* callback2(unsigned char* filtered_buffer, size_t index) {
	//printf("***Here we got into callback2()***\nThe parameters are: filtered_buffer:%p index:%lu\n", filtered_buffer, index);
	unsigned char length_field[4] = { 0,0,0,0 };
	int sequence = 0;
	sequence = buffer_p2c[index].seq;
	memcpy(length_field, buffer_p2c[index].buf + 33, 4);

	//printf("***Here we got the length copied from buffer_p2c in callback2()***\nThe seq num is:%d\n", sequence);

	unsigned long source_len;
	source_len = (256 * 256 * 256 * length_field[0])
		+ (256 * 256 * length_field[1])
		+ (256 * length_field[2])
		+ length_field[3];


	//U8 * source
	unsigned char* tmp_buffer = calloc((size_t)(source_len), 1);
	//read the IDAT data field.
	memcpy(tmp_buffer, buffer_p2c[index].buf + 41, (size_t)source_len);

	//printf("***Here we got the compressed data copied from buffer_p2c in callback2()***\n");

	unsigned long dlen = 0;
	//U8 * dest
	unsigned char* tmp_filtered = calloc((size_t)(6 * (400 * 4 + 1)), 1);
	mem_inf(tmp_filtered, &dlen, tmp_buffer, source_len);
	/*should get dlen == (tmp_height[counter - 1]) * (width * 4 + 1)*/
	//printf("***Here we got the compressed data de-compressed***\n");
	//Till now, we get 1 file's uncompressed & filtered data.

	//copy the data to filtered_buffer
	unsigned int tmp_counter;
	for (tmp_counter = 0; tmp_counter < (6 * (400 * 4 + 1)); tmp_counter++) {
		filtered_buffer[(size_t)(sequence * (6 * (400 * 4 + 1)) + tmp_counter)] = tmp_filtered[tmp_counter];
	}
	//printf("***Here we got the de-compressed data copied into filtered_buffer***\n");
	free(tmp_filtered);
	tmp_filtered = NULL;
	free(tmp_buffer);
	tmp_buffer = NULL;
	//printf("***Gonna leave callback2()***\n");
	return NULL;
}

//the catpng() function below creates all.png using data stored in filtered_buffer
void catpng(unsigned char* filtered_buffer) {
	unsigned int counter = 1;
	unsigned int buff_counter = 0;

	// filtered_buffer -> U8 * source

	// compressed_buffer -> U8 * dest -> compressed & filtered
	unsigned char* compressed_buffer = calloc((size_t)(300 * (400 * 4 + 1)), 1);
	// tmp -> U64 * dest length -> likely to be smaller than source_len
	unsigned long tmp = 0;
	mem_def(compressed_buffer, &tmp, filtered_buffer, (300 * (400 * 4 + 1)), Z_DEFAULT_COMPRESSION);


	//Finally, we could create the "all.png"
	FILE* fpall;
	fpall = fopen("all.png", "wb+");

	// Header -> 8 bytes
	unsigned char header[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
	fwrite(header, 1, 8, fpall);

	// Begin of IHDR

	//Length -> 4 bytes
	unsigned char length_field[4] = { 0x00, 0x00, 0x00, 0x0D };
	fwrite(length_field, 1, 4, fpall);

	// Type and Data Field -> 17 bytes
	unsigned char tnd_field[17];
	//type -> 4 bytes
	tnd_field[0] = 0x49;
	tnd_field[1] = 0x48;
	tnd_field[2] = 0x44;
	tnd_field[3] = 0x52;
	//data field -> 13 bytes
	unsigned int wid, hei;
	wid = htonl(400);
	hei = htonl(300);
	memcpy(tnd_field + 4, &wid, 4);
	memcpy(tnd_field + 8, &hei, 4);
	tnd_field[12] = 0x08;
	tnd_field[13] = 0x06;
	tnd_field[14] = 0x00;
	tnd_field[15] = 0x00;
	tnd_field[16] = 0x00;

	// IHDR's CRC
	fwrite(tnd_field, 1, 17, fpall);
	unsigned long tcrc = 0;
	tcrc = crc(tnd_field, 17);
	unsigned int fcrc = htonl(tcrc);
	fwrite(&fcrc, 4, 1, fpall);

	// IDAT
	unsigned int ftmp = htonl(tmp);
	fwrite(&ftmp, 4, 1, fpall);
	//should be IDAT_tnd?
	unsigned char* IEND_tnd = calloc((size_t)(tmp + 4), 1);
	IEND_tnd[0] = 0x49;
	IEND_tnd[1] = 0x44;
	IEND_tnd[2] = 0x41;
	IEND_tnd[3] = 0x54;
	buff_counter = 0;
	for (buff_counter = 0; buff_counter < tmp; buff_counter++) {
		IEND_tnd[4 + buff_counter] = compressed_buffer[buff_counter];
	}
	fwrite(IEND_tnd, 1, (size_t)(tmp + 4), fpall);
	tcrc = 0;
	fcrc = 0;
	tcrc = crc(IEND_tnd, (int)(tmp + 4));
	fcrc = htonl(tcrc);
	fwrite(&fcrc, 4, 1, fpall);

	// IEND -> 12 bytes
	header[0] = 0x00;
	header[1] = 0x00;
	header[2] = 0x00;
	header[3] = 0x00;
	header[4] = 0x49;
	header[5] = 0x45;
	header[6] = 0x4e;
	header[7] = 0x44;
	fwrite(header, 1, 8, fpall);
	tcrc = 0;
	fcrc = 0;
	tcrc = crc((header + 4), 4);
	fcrc = htonl(tcrc);
	fwrite(&fcrc, 4, 1, fpall);

	//End. -------------------------

	free(IEND_tnd);
	IEND_tnd = NULL;
	free(compressed_buffer);
	compressed_buffer = NULL;
	fclose(fpall);
}

pid_t proc_creator(int buf_size,  int prod_count, int cons_count, int sleep_time, int image_num) { // this function creates all
    // CHILD processes for producers and consumers, and after all is done,
    // use the PARENT process to finally produce the all.png
    // proc_num = producer_num + consumer_num
    
    int proc_num = prod_count + cons_count;
    pid_t pid = 0;
    pid_t cpids[proc_num];
    int state;
    
    for (int i = 0; i < proc_num; i++) {

        pid = fork();

        if ( pid > 0 ) {        /* parent proc */
            cpids[i] = pid;
        } else if ( pid == 0 ) { /* child proc */
			if (i < prod_count) { /*producer process*/
				//printf("A child producer process is running\n");
				while (true) {
					unsigned int part_num = 0;
					unsigned int local_server_num = 0;
					sem_wait(mut_pro);
					//printf("A child Producer(P) process has started using mut_pro. The interested values are:\n*total_frags_produced:%u\npart_num:%u\n",
						//*total_frags_produced, part_num);
					if ((*total_frags_produced) >= 50) { sem_post(mut_pro); break; }//actually we should only have total_frags_produced=50 if things go as expected
					part_num = *total_frags_produced;
					(*total_frags_produced)++;
					local_server_num = (*server_num) + 1;
					*server_num = ((*server_num) + 1) % 3;
					//printf("A child Producer(P) process has ended using mut_pro. The interested values are:\n*total_frags_produced:%u\npart_num:%u\n",
						//*total_frags_produced, part_num);
					sem_post(mut_pro);
					//printf("~~~~~~~~~~~~~~~~A child Producer(P) process is waiting for spaces...\n");
					sem_wait(spaces);
					//printf("!!!!!!!!!!!!!!!!A child Producer(P) process has waited spaces and am trying to get into crit. sec...\n");


					//sem_wait(mut);
					//printf("+++++++++++++++++A child Producer(P) process has got into crit. sec...\n");
					//access shm
					char URL[100];
					sprintf(URL, "http://ece252-%u.uwaterloo.ca:2530/image?img=%d&part=%u", local_server_num, image_num, part_num);
					//printf("^^^^^^^^^^^^^^^^^^^^^^^^^^^URL is %s\n", URL);
					//notice here *server_num is between 0 and 2, so we need to add 1 to it for the actual server num
					callback(URL, mut, producer_index, buf_size);
					/*callback() above should have fetched the image strip from the server and put it into buffer_p2c[*producer_index]*/
					//(*producer_index) = ((*producer_index) + 1) % buf_size;
					//printf("-------------------A child Producer(P) process is about to leave crit. sec...\n");
					//sem_post(mut);


					//printf("-------------------A child Producer(P) process is about to post item\n");
					sem_post(items);
				}
				//printf("A child producer process has stopped running\n");
				break;
			}
			else { /*consumer process*/
				//printf("A child consumer process is running\n");
				while (true) {
					sem_wait(mut_con);
					//printf("A child Consumer(C) process has ended using mut_con. The interested values are:\n*total_frags_consumed:%u\n",
						//*total_frags_consumed);
					if ((*total_frags_consumed) >= 50) { sem_post(mut_con); break; }//actually we should only have total_frags_produced=50 if things go as expected
					(*total_frags_consumed)++;
					//printf("A child Consumer(C) process has ended using mut_con. The interested values are:\n*total_frags_consumed:%u\n",
						//*total_frags_consumed);
					sem_post(mut_con);

					usleep(1000 * sleep_time);//sleep to simulate processing
					//printf("~~~~~~~~~~~~~~~~A child Consumer(C) process has finished sleeping and is waiting for items...\n");
					sem_wait(items);
					//printf("!!!!!!!!!!!!!!!!A child Consumer(C) process has waited items and am trying to get into crit. sec...\n");
					sem_wait(mut);
					//printf("+++++++++++++++++A child Consumer(C) process has got into crit. sec...\n");
					//access shm
					callback2(filtered_buffer, (*consumer_index));
					//no longer needed -> remember to free char* buf in buffer_p2c!!!!!!!!
					//no longer needed -> recv_buf_cleanup(buffer_p2c + consumer_index);
					//after accessed:
					(*consumer_index) = ((*consumer_index) + 1) % buf_size;
					//printf("-------------------A child Consumer(C) process is about to leave crit. sec...\n");
					sem_post(mut);
					//printf("-------------------A child Consumer(C) process is about to post spaces\n");
					sem_post(spaces);
				}
				//printf("A child consumer process has stopped running\n");
				break;
			}
        } else {
			/*sth. went wrong with fork*/
            perror("fork");
            abort();
        }

    }
    
    if (pid > 0) {
        for (int i = 0; i < proc_num; i++) {
            waitpid(cpids[i], &state, 0);
        }
        catpng(filtered_buffer);
        // produce the all.png
    }

	return pid;
}


int main(int argc, char *argv[]) {
	//printf("At least we got the main process started\n");

    int buf_size, prod_count, cons_count, sleep_time, image_num, proc_num;
//    int state;

    option_parser(argv, &buf_size, &prod_count, &cons_count, &sleep_time, &image_num);
    proc_num = prod_count + cons_count;

	//printf("The options parsed are: buf_size:%d, prod_count:%d, cons_count:%d, sleep_time:%d, image_num:%d, proc_num:%d\n",
		//buf_size, prod_count, cons_count, sleep_time, image_num, proc_num);

	int shmid_filtered_buffer, shmid_total_frags_produced, shmid_total_frags_consumed, shmid_server_num, shmid_buffer_p2c;

	int shmid_spaces, shmid_items, shmid_mut, shmid_mut_pro, shmid_mut_con;

	int shmid_producer_index, shmid_consumer_index;

	shmid_spaces = shmget(IPC_PRIVATE, sizeof(sem_t), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
	shmid_items = shmget(IPC_PRIVATE, sizeof(sem_t), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
	shmid_mut = shmget(IPC_PRIVATE, sizeof(sem_t), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
	shmid_mut_pro = shmget(IPC_PRIVATE, sizeof(sem_t), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
	shmid_mut_con = shmget(IPC_PRIVATE, sizeof(sem_t), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
	shmid_producer_index = shmget(IPC_PRIVATE, sizeof(size_t), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
	shmid_consumer_index = shmget(IPC_PRIVATE, sizeof(size_t), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);

	//printf("Shared memory reserved for semaphores & indexes created\n");

	spaces = shmat(shmid_spaces, NULL, 0);
	items = shmat(shmid_items, NULL, 0);
	mut = shmat(shmid_mut, NULL, 0);
	mut_pro = shmat(shmid_mut_pro, NULL, 0);
	mut_con = shmat(shmid_mut_con, NULL, 0);
	producer_index = shmat(shmid_producer_index, NULL, 0);
	consumer_index = shmat(shmid_consumer_index, NULL, 0);

	//printf("Semaphores & indexes attached to shared memory\n");

	sem_init(spaces, 1, buf_size);
	sem_init(items, 1, 0);
	sem_init(mut, 1, 1);
	sem_init(mut_pro, 1, 1);
	sem_init(mut_con, 1, 1);
	*producer_index = 0;
	*consumer_index = 0;
	//printf("Semaphores initialized\n");

	shmid_filtered_buffer = shmget(IPC_PRIVATE, (size_t)(300 * (400 * 4 + 1)), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
	shmid_total_frags_produced = shmget(IPC_PRIVATE, sizeof(unsigned int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
	shmid_total_frags_consumed = shmget(IPC_PRIVATE, sizeof(unsigned int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
	shmid_server_num = shmget(IPC_PRIVATE, sizeof(unsigned int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);
	shmid_buffer_p2c = shmget(IPC_PRIVATE, (size_t)(buf_size * sizeof(RECV_BUF_FIXED)), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR);

	//printf("shmget() run. The shmids are: shmid_filtered_buffer:%d, shmid_total_frags_produced:%d, shmid_total_frags_consumed:%d, shmid_server_num:%d, shmid_buffer_p2c:%d\n",
		//shmid_filtered_buffer, shmid_total_frags_produced, shmid_total_frags_consumed, shmid_server_num, shmid_buffer_p2c);

	filtered_buffer = shmat(shmid_filtered_buffer, NULL, 0);
	total_frags_produced = shmat(shmid_total_frags_produced, NULL, 0);
	total_frags_consumed = shmat(shmid_total_frags_consumed, NULL, 0);
	server_num = shmat(shmid_server_num, NULL, 0);
	buffer_p2c = shmat(shmid_buffer_p2c, NULL, 0);
	*total_frags_produced = 0;
	*total_frags_consumed = 0;
	*server_num = 0;

	//printf("Shm pointers attached. They are:\nfiltered_buffer:%p\ntotal_frags_produced:%p\ntotal_frags_consumed:%p\nserver_num:%p\nbuffer_p2c:%p\n",
		/*filtered_buffer,
		total_frags_produced,
		total_frags_consumed,
		server_num,
		buffer_p2c);*/

	/*filtered_buffer = calloc((size_t)(300 * (400 * 4 + 1)), 1);//maybe transfrom into shm later if this way leads to memory leak
	total_frags_produced = calloc(1, sizeof(unsigned int));//maybe transfrom into shm later if this way leads to memory leak
	total_frags_consumed = calloc(1, sizeof(unsigned int));//maybe transfrom into shm later if this way leads to memory leak
	server_num = calloc(1, sizeof(unsigned int));//maybe transfrom into shm later if this way leads to memory leak
	buffer_p2c = calloc(buf_size, sizeof(RECV_BUF));//maybe transfrom into shm later if this way leads to memory leak*/
	double times[2];
	struct timeval tv;
	if (gettimeofday(&tv, NULL) != 0) {
		perror("gettimeofday");
		abort();
	}
	times[0] = (tv.tv_sec) + tv.tv_usec / 1000000.;
	//record the start time
	pid_t main_pid;
	main_pid = proc_creator(buf_size, prod_count, cons_count, sleep_time, image_num);

	shmdt(filtered_buffer);
	shmdt(total_frags_produced);
	shmdt(total_frags_consumed);
	shmdt(server_num);
	shmdt(buffer_p2c);

	shmdt(spaces);
	shmdt(items);
	shmdt(mut);
	shmdt(mut_pro);
	shmdt(mut_con);
	shmdt(producer_index);
	shmdt(consumer_index);
	//semaphores & indexes detached in all processes
	if (main_pid > 0) {//do clean-up work in the parent process
		if (gettimeofday(&tv, NULL) != 0) {
			perror("gettimeofday");
			abort();
		}
		times[1] = (tv.tv_sec) + tv.tv_usec / 1000000.;
		printf("paster2 execution time: %.6lf seconds\n", times[1] - times[0]);
		//record end time and print out
		shmctl(shmid_filtered_buffer, IPC_RMID, NULL);
		shmctl(shmid_total_frags_produced, IPC_RMID, NULL);
		shmctl(shmid_total_frags_consumed, IPC_RMID, NULL);
		shmctl(shmid_server_num, IPC_RMID, NULL);
		shmctl(shmid_buffer_p2c, IPC_RMID, NULL);
		shmctl(shmid_producer_index, IPC_RMID, NULL);
		shmctl(shmid_consumer_index, IPC_RMID, NULL);
		//destroy the semaphores & indexes
		sem_destroy(spaces);
		sem_destroy(items);
		sem_destroy(mut);
		sem_destroy(mut_pro);
		sem_destroy(mut_con);
		//delete the shared memory reserved for semaphores
		shmctl(shmid_spaces, IPC_RMID, NULL);
		shmctl(shmid_items, IPC_RMID, NULL);
		shmctl(shmid_mut, IPC_RMID, NULL);
		shmctl(shmid_mut_pro, IPC_RMID, NULL);
		shmctl(shmid_mut_con, IPC_RMID, NULL);
		//printf("parent done work wrapping up");
	}
	spaces = NULL;
	items = NULL;
	mut = NULL;
	mut_pro = NULL;
	mut_con = NULL;
	/*free(total_frags_produced);
	free(total_frags_consumed);
	free(server_num);
	free(buffer_p2c);
	free(filtered_buffer);*/
	total_frags_produced = NULL;
	total_frags_consumed = NULL;
	server_num = NULL;
	buffer_p2c = NULL;
	filtered_buffer = NULL;
}
