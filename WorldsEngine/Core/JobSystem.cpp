#include "JobSystem.hpp"
#include <iostream>
#ifdef TRACY_ENABLE
#include "tracy/Tracy.hpp"
#endif
#ifdef __linux__
#include <pthread.h>
#endif

namespace worlds {
    JobSystem::JobSystem(int numWorkers)
        : executing(true) {
        jobListsMutex.lock();
        for (int i = 0; i < numWorkers; i++) {
            auto t = std::thread(&JobSystem::worker, this, i);
#ifdef __linux__
            pthread_setname_np(t.native_handle(), "worker thread");
#endif
            workers.push_back(std::move(t));
        }

        for (int i = 0; i < NUM_JOB_SLOTS; i++) {
            currentJobLists[i].completed = true;
            currentJobLists[i].completeCV = SDL_CreateCond();
            currentJobLists[i].completeMutex = SDL_CreateMutex();
        }
        jobListsMutex.unlock();

        // Wait for a little bit so all the threads are ready to receive
        // jobs
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    JobSystem::~JobSystem() {
        executing = false;

        for (int i = 0; i < NUM_JOB_SLOTS; i++) {
            SDL_DestroyCond(currentJobLists[i].completeCV);
            SDL_DestroyMutex(currentJobLists[i].completeMutex);
        }

        newJobListCV.notify_all();
        for (auto& w : workers) {
            w.join();
        }
    }

    JobList& JobSystem::getFreeJobList() {
        return currentJobLists[getFreeJobSlot()];
    }

    void JobSystem::signalJobListAvailable() {
        newJobListCV.notify_all();
    }

    void JobSystem::completeFrameJobs() {
        for (auto& jobList : currentJobLists) {
            if (!jobList.completed)
                jobList.wait();
        }
    }

    int JobSystem::getFreeJobSlot() {
        for (int i = 0; i < NUM_JOB_SLOTS; i++) {
            if (currentJobLists[i].completed)
                return i;
        }
        return -1;
    }

    void JobSystem::worker(int idx) {
#ifdef TRACY_ENABLE
        ZoneScoped;
#endif
        while (executing) {
            // Get a job from any of the job lists
            jobListsMutex.lock();
            Job currentJob(nullptr);
            JobList* pulledFromList = nullptr;

            for (auto& jobList : currentJobLists) {
                if (!jobList.completed && !jobList.jobs.empty()) {
                    currentJob = jobList.jobs.front();
                    jobList.jobs.pop();
                    pulledFromList = &jobList;
                }
            }
            jobListsMutex.unlock();

            // No jobs available :(
            if (pulledFromList == nullptr) {
                // Wait for any new job lists
                {
                    std::unique_lock<std::mutex> newJobListLck(newJobListM);
                    newJobListCV.wait(newJobListLck);
                }

                // Check if the job list was actually just
                // the program terminating
                if (!executing)
                    return;
                continue;
            }

            // Run the job
            currentJob.function();
            if (currentJob.completeFunc)
                currentJob.completeFunc();

            jobListsMutex.lock();
            // Check if we just completed the list
            pulledFromList->completedJobs.fetch_add(1);
            if (pulledFromList->completedJobs.load() >= pulledFromList->startJobCount) {
                SDL_LockMutex(pulledFromList->completeMutex);
                pulledFromList->completed = true;
                SDL_CondBroadcast(pulledFromList->completeCV);
                SDL_UnlockMutex(pulledFromList->completeMutex);
            }
            jobListsMutex.unlock();
        }
    }
}
