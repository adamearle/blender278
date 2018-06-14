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
*		Cristian Kovacs (Only the Multi Add Node)
*/

#include "COM_MultiAddOperation.h"

extern "C" {
#  include "BLI_math.h"
}

/* ******** Multi Add Operation ******** */
MultiAddOperation::MultiAddOperation(size_t num_inputs) : NodeOperation()
{
	for (size_t i = 0; i < num_inputs; i++) {
		this->addInputSocket(COM_DT_COLOR);
	}
	inputs.resize(num_inputs);
	this->addOutputSocket(COM_DT_COLOR);
	this->setUseClamp(false);
}

void MultiAddOperation::initExecution()
{
	for (size_t i = 0; i < inputs.size(); i++) {
		inputs[i] = this->getInputSocketReader(i);
	}
}

void MultiAddOperation::executePixelSampled(float output[4], float x, float y, PixelSampler sampler)
{
	int arrsize = inputs.size();
	float inputValue[4];
	float inputColor[4];

	for (size_t i = 0; i < inputs.size(); i++) {
		inputs[i]->readSampled(inputColor, x, y, sampler);
		if (i == 0) {
			output[0] = inputColor[0];
			output[1] = inputColor[1];
			output[2] = inputColor[2];
			output[3] = inputColor[3];
		}
		else {
			output[0] += inputColor[0];
			output[1] += inputColor[1];
			output[2] += inputColor[2];
		}
	}

	clampIfNeeded(output);
}

void MultiAddOperation::determineResolution(unsigned int resolution[2], unsigned int preferredResolution[2])
{
	NodeOperationInput *socket;
	unsigned int tempPreferredResolution[2] = { 0, 0 };
	unsigned int tempResolution[2];

	for (size_t i = 0; i < inputs.size(); i++) {
		socket = this->getInputSocket(i);
		socket->determineResolution(tempResolution, tempPreferredResolution);
		if ((tempResolution[0] != 0) && (tempResolution[1] != 0)) {
			this->setResolutionInputSocketIndex(i);
			break;
		}
	}
	NodeOperation::determineResolution(resolution, preferredResolution);
}

void MultiAddOperation::deinitExecution()
{
	for (size_t i = 0; i < inputs.size(); i++) {
		inputs[i] = NULL;
	}
}