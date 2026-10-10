#pragma once

int& TestFailures();
void Expect(bool cond, const char* name);

void RunGainTests();
void RunEngineTests();
void RunUiMathTests();
void RunMeterTests();
