
#include "score.h"
#include <stdio.h>

static int score = 0;

///variables for moving average
static double values[2][15] = {0};
static int pos[2] = {0};
static int len[2] = {sizeof(values[0]) / sizeof(double), sizeof(values[1]) / sizeof(double)};
static double avg[2] = {0};
static double sum[2]= {0};


int get_score(void){
    return score;
}

void reset_score(void){
    score = 0;
}

void modify_score(int modifier){
    score += modifier;
}

void moving_avg(double value, int id){

    sum[id] = sum[id] - values[id][pos[id]] + value;
    values[id][pos[id]] = value;
    pos[id]++;
    if(pos[id] >= len[id]){
        pos[id] = 0;
        avg[id] = sum[id]/len[id];
        evaluate_avg(id);
    }
}

void evaluate_avg(int id){
    int modifier;
    double threshold = (id == BUBBLE_TURNAROUND_ID) ? BUBBLE_TURNAROUND_AVG_THRESHOLD : EMPTY_PT_AVG_THRESHOLD;

    if(avg[id]>threshold){
        //printf("id:%d\n",id);
        modifier = (id == BUBBLE_TURNAROUND_ID) ? BUBBLE_TURNAROUND_MODIFIER : EMPTY_PT_MODIFIER;
        modify_score(modifier);
    }
}

void post_stragglers_percentage(double percentage){
    if(percentage > STRAGGLERS_RATE_THRESHOLD){
        modify_score(STRAGGLERS_RATE_MODIFIER);
    }
}