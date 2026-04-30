/*
 * Project: Crankshaft
 * This file is part of Crankshaft project.
 * Copyright (C) 2025 OpenCarDev Team
 *
 *  Crankshaft is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Crankshaft is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Crankshaft. If not, see <http://www.gnu.org/licenses/>.
 */

#include "CoreApplicationRunner.h"

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

// Generated build info
const char CRANKSHAFT_BUILD_TIMESTAMP[] = TOSTRING(CRANKSHAFT_BUILD_TIMESTAMP_VALUE);
const char CRANKSHAFT_GIT_COMMIT_SHORT[] = TOSTRING(CRANKSHAFT_GIT_COMMIT_SHORT_VALUE);
const char CRANKSHAFT_GIT_COMMIT_LONG[] = TOSTRING(CRANKSHAFT_GIT_COMMIT_LONG_VALUE);
const char CRANKSHAFT_GIT_BRANCH[] = TOSTRING(CRANKSHAFT_GIT_BRANCH_VALUE);

int main(int argc, char* argv[]) {
  CoreBuildInfo buildInfo{CRANKSHAFT_BUILD_TIMESTAMP, CRANKSHAFT_GIT_COMMIT_SHORT,
                          CRANKSHAFT_GIT_COMMIT_LONG, CRANKSHAFT_GIT_BRANCH};
  return runCoreApplication(argc, argv, buildInfo);
}
