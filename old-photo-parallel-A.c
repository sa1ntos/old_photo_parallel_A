#include <pthread.h>
#include <gd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "image-lib.h"

#define MAX_FILES 1000

typedef struct
{
    char **files;
    int start_idx;
    int end_idx;
    gdImagePtr texture_img;
    char *output_dir;
    struct timespec start_time;
    struct timespec end_time;
} ThreadData;

void divide_workload(int total_images, int num_threads, ThreadData *thread_data, char **files, char *output_dir, gdImagePtr texture_img)
{
    int base_workload = total_images / num_threads;
    int resto = total_images % num_threads;
    int start_idx = 0;

    for (int i = 0; i < num_threads; i++)
    {
        thread_data[i].start_idx = start_idx;
        thread_data[i].end_idx = start_idx + base_workload + (i < resto ? 1 : 0); // Distribui o resto
        thread_data[i].files = files;
        thread_data[i].output_dir = output_dir;
        thread_data[i].texture_img = texture_img;
        start_idx = thread_data[i].end_idx;
    }
}

void *process_images(void *arg)
{
    ThreadData *data = (ThreadData *)arg;
    clock_gettime(CLOCK_MONOTONIC, &data->start_time);

    char out_file_name[200];
    for (int i = data->start_idx; i < data->end_idx; i++)
    {
        snprintf(out_file_name, sizeof(out_file_name), "%s/%s", data->output_dir, strrchr(data->files[i], '/') + 1);

        if (access(out_file_name, F_OK) != -1)
        {
            continue;
        }

        gdImagePtr in_img = read_jpeg_file(data->files[i]);
        if (!in_img)
        {
            fprintf(stderr, "Erro ao ler %s\n", data->files[i]);
            continue;
        }

        gdImagePtr out_contrast_img = contrast_image(in_img);
        gdImagePtr out_smoothed_img = smooth_image(out_contrast_img);
        gdImagePtr out_textured_img = texture_image(out_smoothed_img, data->texture_img);
        gdImagePtr out_sepia_img = sepia_image(out_textured_img);

        if (write_jpeg_file(out_sepia_img, out_file_name) == 0)
        {
            fprintf(stderr, "Erro ao salvar %s\n", out_file_name);
        }

        gdImageDestroy(out_smoothed_img);
        gdImageDestroy(out_sepia_img);
        gdImageDestroy(out_contrast_img);
        gdImageDestroy(in_img);
    }

    clock_gettime(CLOCK_MONOTONIC, &data->end_time);
    pthread_exit(NULL);
}

int compare_name(const void *a, const void *b)
{
    const char *file1 = *(const char **)a;
    const char *file2 = *(const char **)b;
    return strcasecmp(file1, file2);
}

int compare_size(const void *a, const void *b)
{
    const char *file1 = *(const char **)a;
    const char *file2 = *(const char **)b;

    struct stat stat1, stat2;
    stat(file1, &stat1);
    stat(file2, &stat2);

    return (stat1.st_size > stat2.st_size) - (stat1.st_size < stat2.st_size);
}

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        fprintf(stderr, "Uso: %s <diretorio> <n_threads> <-name|-size>\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *input_dir = argv[1];
    int n_threads = atoi(argv[2]);
    char *sort_type = argv[3];

    struct timespec start_time_total, end_time_total;
    struct timespec start_time_seq, end_time_seq;
    struct timespec start_time_par, end_time_par;

    clock_gettime(CLOCK_MONOTONIC, &start_time_total);

    struct dirent *entry;
    char *files[MAX_FILES];
    int file_count = 0;

    DIR *dir = opendir(input_dir);
    if (!dir)
    {
        perror("Erro ao abrir diretório");
        return EXIT_FAILURE;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        if (strstr(entry->d_name, ".jpeg"))
        {
            files[file_count] = malloc(strlen(input_dir) + strlen(entry->d_name) + 2);
            sprintf(files[file_count], "%s/%s", input_dir, entry->d_name);
            file_count++;
        }
    }
    closedir(dir);

    if (file_count == 0)
    {
        fprintf(stderr, "Nenhum arquivo JPEG encontrado no diretório %s.\n", input_dir);
        return EXIT_FAILURE;
    }

    clock_gettime(CLOCK_MONOTONIC, &start_time_seq);
    if (strcmp(sort_type, "-name") == 0)
    {
        qsort(files, file_count, sizeof(char *), compare_name);
    }
    else if (strcmp(sort_type, "-size") == 0)
    {
        qsort(files, file_count, sizeof(char *), compare_size);
    }
    else
    {
        fprintf(stderr, "Erro: argumento de ordenação inválido. Use -name ou -size.\n");
        return EXIT_FAILURE;
    }
    clock_gettime(CLOCK_MONOTONIC, &end_time_seq);

    char output_dir[200];
    snprintf(output_dir, sizeof(output_dir), "%s/old_photo_PAR_A", input_dir);
    if (access(output_dir, F_OK) != 0)
    {
        if (mkdir(output_dir, 0755) != 0)
        {
            perror("Erro ao criar diretório de saída");
            return EXIT_FAILURE;
        }
    }

    gdImagePtr texture_img = read_png_file("paper-texture.png");
    if (!texture_img)
    {
        fprintf(stderr, "Erro ao carregar a textura.\n");
        return EXIT_FAILURE;
    }

    clock_gettime(CLOCK_MONOTONIC, &start_time_par);
    pthread_t threads[n_threads];
    ThreadData thread_data[n_threads];

    if (n_threads > 0)
    {
        divide_workload(file_count, n_threads, thread_data, files, output_dir, texture_img);

        for (int i = 0; i < n_threads; i++)
        {
            pthread_create(&threads[i], NULL, process_images, &thread_data[i]);
        }

        for (int i = 0; i < n_threads; i++)
        {
            pthread_join(threads[i], NULL);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end_time_par);
    clock_gettime(CLOCK_MONOTONIC, &end_time_total);

    struct timespec seq_time = diff_timespec(&end_time_seq, &start_time_seq);
    struct timespec par_time = diff_timespec(&end_time_par, &start_time_par);
    struct timespec total_time = diff_timespec(&end_time_total, &start_time_total);

    char timing_file[300];
    snprintf(timing_file, sizeof(timing_file), "%s/timing_%d-%s.txt", output_dir, n_threads, (strcmp(sort_type, "-name") == 0) ? "name" : "size");
    FILE *fp = fopen(timing_file, "w");
    if (!fp)
    {
        perror("Erro ao criar o arquivo de tempos");
        return EXIT_FAILURE;
    }

    fprintf(fp, "Tempo total: %ld.%09ld segundos\n", total_time.tv_sec, total_time.tv_nsec);
    fprintf(fp, "Tempo sequencial: %ld.%09ld segundos\n", seq_time.tv_sec, seq_time.tv_nsec);

    if (n_threads > 0)
    {
        for (int i = 0; i < n_threads; i++)
        {
            struct timespec thread_time = diff_timespec(&thread_data[i].end_time, &thread_data[i].start_time);
            fprintf(fp, "Thread %d: %ld.%09ld segundos\n", i, thread_time.tv_sec, thread_time.tv_nsec);
        }
    }

    fclose(fp);
    printf("Parallel execution:\n");
    printf("\tseq \t %10ld.%09ld\n", seq_time.tv_sec, seq_time.tv_nsec);
    printf("\tpar \t %10ld.%09ld\n", par_time.tv_sec, par_time.tv_nsec);
    printf("\ttotal \t %10ld.%09ld\n", total_time.tv_sec, total_time.tv_nsec);

    gdImageDestroy(texture_img);
    for (int i = 0; i < file_count; i++)
    {
        free(files[i]);
    }

    return EXIT_SUCCESS;
}
