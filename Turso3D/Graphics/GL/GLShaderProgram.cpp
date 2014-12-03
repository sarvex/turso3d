﻿// For conditions of distribution and use, see copyright notice in License.txt

#include "../../Debug/Log.h"
#include "../../Debug/Profiler.h"
#include "GLGraphics.h"
#include "GLShaderProgram.h"
#include "GLShaderVariation.h"
#include "GLVertexBuffer.h"

#include <flextGL.h>

#include "../../Debug/DebugNew.h"

namespace Turso3D
{

const size_t MAX_NAME_LENGTH = 256;

ShaderProgram::ShaderProgram(ShaderVariation* vs_, ShaderVariation* ps_) :
    program(0),
    vs(vs_),
    ps(ps_)
{
}

ShaderProgram::~ShaderProgram()
{
    Release();
}

void ShaderProgram::Release()
{
    if (program)
    {
        glDeleteProgram(program);
        program = 0;
    }
}

bool ShaderProgram::Link()
{
    PROFILE(LinkShaderProgram);

    Release();

    if (!graphics || !graphics->IsInitialized())
    {
        LOGERROR("Can not link shader program without initialized Graphics subsystem");
        return false;
    }
    if (!vs || !ps)
    {
        LOGERROR("Shader(s) are null, can not link shader program");
        return false;
    }
    if (!vs->ShaderObject() || !ps->ShaderObject())
    {
        LOGERROR("Shaders have not been compiled, can not link shader program");
        return false;
    }

    program = glCreateProgram();
    if (!program)
    {
        LOGERROR("Could not create shader program");
        return false;
    }

    glAttachShader(program, vs->ShaderObject());
    glAttachShader(program, ps->ShaderObject());
    glLinkProgram(program);

    int linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        int length, outLength;
        String errorString;

        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        errorString.Resize(length);
        glGetProgramInfoLog(program, length, &outLength, &errorString[0]);
        glDeleteProgram(program);
        program = 0;

        LOGERRORF("Could not link shaders %s: %s", FullName().CString(), errorString.CString());
        return false;
    }

    LOGDEBUGF("Linked shaders %s", FullName().CString());

    glUseProgram(program);

    char nameBuffer[MAX_NAME_LENGTH];
    int numAttributes, numUniforms, numUniformBlocks, nameLength, numElements;
    GLenum type;

    attributes.Clear();

    glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &numAttributes);
    for (int i = 0; i < numAttributes; ++i)
    {
        glGetActiveAttrib(program, i, (GLsizei)MAX_NAME_LENGTH, &nameLength, &numElements, &type, nameBuffer);

        String name(nameBuffer, nameLength);
        const char** semantics = VertexBuffer::elementSemantic;
        Pair<ElementSemantic, unsigned char> newAttribute;
        newAttribute.first = SEM_POSITION;
        newAttribute.second = 0;

        while (*semantics)
        {
            if (name.StartsWith(*semantics, false))
            {
                String indexStr = name.Substring(String::CStringLength(*semantics));
                if (indexStr.Length())
                    newAttribute.second = (unsigned char)indexStr.ToInt();
                break;
            }
            newAttribute.first = (ElementSemantic)(newAttribute.first + 1);
            ++semantics;
        }

        if (newAttribute.first == SEM_UNKNOWN)
            LOGWARNINGF("Found vertex attribute %s with no known semantic in shader program %s", name.CString(), FullName().CString());
        
        attributes.Push(newAttribute);
    }

    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &numUniforms);
    for (int i = 0; i < numUniforms; ++i)
    {
        glGetActiveUniform(program, i, MAX_NAME_LENGTH, &nameLength, &numElements, &type, nameBuffer);

        String name(nameBuffer, nameLength);
        if (type >= GL_SAMPLER_1D && type <= GL_SAMPLER_2D_SHADOW)
        {
            // Assign sampler uniforms to a texture unit according to the number appended to the sampler name
            int location = glGetUniformLocation(program, name.CString());
            int unit = name.ToInt();
            glUniform1iv(location, 1, &unit);
        }
    }

    glGetProgramiv(program, GL_ACTIVE_UNIFORM_BLOCKS, &numUniformBlocks);
    for (int i = 0; i < numUniformBlocks; ++i)
    {
        glGetActiveUniformBlockName(program, i, (GLsizei)MAX_NAME_LENGTH, &nameLength, nameBuffer);
        
        String name(nameBuffer, nameLength);
        //LOGINFOF("Uniform block %d: %s", i, name.CString());
    }

    return true;
}

ShaderVariation* ShaderProgram::VertexShader() const
{
    return vs;
}

ShaderVariation* ShaderProgram::PixelShader() const
{
    return ps;
}

String ShaderProgram::FullName() const
{
    return (vs && ps) ? vs->FullName() + " " + ps->FullName() : String::EMPTY;
}

}