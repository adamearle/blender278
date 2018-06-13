/*
* Copyright 2011, Blender Foundation.
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software Foundation,
* Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*
* Contributor:
*		Jeroen Bakker
*		Monique Dewanchand
*		Cristian Kovacs (only the multi mix node)
*/

#ifndef _COM_MultiMixOperation_h
#define _COM_MultiMixOperation_h
#include "COM_NodeOperation.h"

class MultiMixOperation : public NodeOperation {
protected:
	/**
	* Prefetched reference to the inputProgram
	*/
	bool m_useClamp;

	inline void clampIfNeeded(float color[4])
	{
		if (m_useClamp) {
			CLAMP(color[0], 0.0f, 1.0f);
			CLAMP(color[1], 0.0f, 1.0f);
			CLAMP(color[2], 0.0f, 1.0f);
			CLAMP(color[3], 0.0f, 1.0f);
		}
	}

public:
	/**
	* Default constructor
	*/
	MultiMixOperation(size_t num_inputs = 2);

	std::vector<SocketReader *> inputs;

	/**
	* the inner loop of this program
	*/
	void executePixelSampled(float output[4], float x, float y, PixelSampler sampler);

	/**
	* Initialize the execution
	*/
	void initExecution();

	/**
	* Deinitialize the execution
	*/
	void deinitExecution();

	void determineResolution(unsigned int resolution[2], unsigned int preferredResolution[2]);

	void setUseClamp(bool value) { this->m_useClamp = value; }
};

#endif