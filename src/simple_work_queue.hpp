#ifndef __SIMPLE_WORK_QUEUE_HPP_
#define __SIMPLE_WORK_QUEUE_HPP_

#include <deque>
#include <pthread.h>

using namespace std;

struct work_queue {
    deque<int> jobs;
    pthread_mutex_t jobs_mutex;
    
    int add_job(int fd, pthread_cond_t * cond) {
        pthread_mutex_lock(&this->jobs_mutex);
        jobs.push_back(fd);
        size_t len = jobs.size();
        pthread_cond_signal(cond);
        pthread_mutex_unlock(&this->jobs_mutex);
        return len;
    }
    
    bool remove_job(int *job) {
        bool success = !this->jobs.empty();
        if (success) {
            *job = this->jobs.front();
            this->jobs.pop_front();
        }
        return success;
    }

    bool remove_job_thread_safe(int *job) {
        pthread_mutex_lock(&this->jobs_mutex);
        bool success = !this->jobs.empty();
        if (success) {
            *job = this->jobs.front();
            this->jobs.pop_front();
        }
        pthread_mutex_unlock(&this->jobs_mutex);
        return success;
    }

    bool is_empty() {
        bool isEmpty = jobs.size() == 0;
        return isEmpty;
    }
};

#endif
