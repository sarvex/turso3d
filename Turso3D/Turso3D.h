// For conditions of distribution and use, see copyright notice in License.txt

#pragma once

// Convenience header file for including commonly needed engine classes. Note: intentionally does not include Debug/DebugNew.h
// so that placement new works as expected.

#include "Base/AutoPtr.h"
#include "Base/HashSet.h"
#include "Base/List.h"
#include "Base/SharedPtr.h"
#include "Debug/Log.h"
#include "IO/CommandLine.h"
#include "IO/Console.h"
#include "IO/File.h"
#include "IO/FileSystem.h"
#include "Math/Frustum.h"
#include "Math/Polyhedron.h"
#include "Math/Ray.h"
#include "Object/Object.h"
#include "Thread/Condition.h"
#include "Thread/Mutex.h"
#include "Thread/Condition.h"
#include "Thread/Mutex.h"
#include "Thread/Thread.h"