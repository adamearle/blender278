/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "device/device.h"
#include "render/image.h"
#include "render/scene.h"

#include "util/util_foreach.h"
#include "util/util_logging.h"
#include "util/util_path.h"
#include "util/util_progress.h"
#include "util/util_texture.h"

#ifdef WITH_OSL
#include <OSL/oslexec.h>
#endif

#include "kernel/kernel_oiio_globals.h"
#include <OpenImageIO/imagebufalgo.h>

CCL_NAMESPACE_BEGIN

ImageManager::ImageManager(const DeviceInfo& info)
{
	need_update = true;
	pack_images = false;
	oiio_texture_system = NULL;
	animation_frame = 0;

	/* In case of multiple devices used we need to know type of an actual
	 * compute device.
	 *
	 * NOTE: We assume that all the devices are same type, otherwise we'll
	 * be screwed on so many levels..
	 */
	DeviceType device_type = info.type;
	if(device_type == DEVICE_MULTI) {
		device_type = info.multi_devices[0].type;
	}

	/* Set image limits */
	max_num_images = TEX_NUM_MAX;
	has_half_images = true;
	cuda_fermi_limits = false;
	
	if(device_type == DEVICE_CUDA) {
		if(!info.has_bindless_textures) {
			cuda_fermi_limits = true;
			has_half_images = false;
		}
	}
	else if(device_type == DEVICE_OPENCL) {
		has_half_images = false;
	}
	
	for(size_t type = 0; type < IMAGE_DATA_NUM_TYPES; type++) {
		tex_num_images[type] = 0;
	}
}

ImageManager::~ImageManager()
{
	for(size_t type = 0; type < IMAGE_DATA_NUM_TYPES; type++) {
		for(size_t slot = 0; slot < images[type].size(); slot++)
			assert(!images[type][slot]);
	}
}

void ImageManager::set_pack_images(bool pack_images_)
{
	pack_images = pack_images_;
}

void ImageManager::set_oiio_texture_system(void *texture_system)
{
	oiio_texture_system = texture_system;
}

bool ImageManager::set_animation_frame_update(int frame)
{
	if(frame != animation_frame) {
		animation_frame = frame;

		for(size_t type = 0; type < IMAGE_DATA_NUM_TYPES; type++) {
			for(size_t slot = 0; slot < images[type].size(); slot++) {
				if(images[type][slot] && images[type][slot]->animated)
					return true;
			}
		}
	}

	return false;
}

device_memory *ImageManager::image_memory(DeviceScene *dscene, int flat_slot)
{
	   ImageDataType type;
	   int slot = flattened_slot_to_type_index(flat_slot, &type);

	   device_memory *tex_img = NULL;

	   switch(type) {
		   case IMAGE_DATA_TYPE_FLOAT4:
			   tex_img = dscene->tex_float4_image[slot];
			   break;
		   case IMAGE_DATA_TYPE_FLOAT:
			   tex_img = dscene->tex_float_image[slot];
			   break;
		   case IMAGE_DATA_TYPE_BYTE:
			   tex_img = dscene->tex_byte_image[slot];
			   break;
		   case IMAGE_DATA_TYPE_USHORT:
			   tex_img = dscene->tex_ushort_image[slot];
			   break;
		   case IMAGE_DATA_TYPE_BYTE4:
			   tex_img = dscene->tex_byte4_image[slot];
			   break;
		   case IMAGE_DATA_TYPE_HALF:
			   tex_img = dscene->tex_half_image[slot];
			   break;
		   case IMAGE_DATA_TYPE_HALF4:
			   tex_img = dscene->tex_half4_image[slot];
			   break;
		   case IMAGE_DATA_TYPE_USHORT4:
			   tex_img = dscene->tex_ushort4_image[slot];
			   break;
		   default:
			   assert(0);
	   }

	   return tex_img;
}

ImageDataType ImageManager::get_image_metadata(const string& filename,
                                               void *builtin_data,
                                               boost::shared_ptr<uint8_t> generated_data,
                                               bool& is_linear)
{
	is_linear = false;
	int channels = 4;
	int channel_size = 0;
	ImageDataType type = IMAGE_DATA_TYPE_BYTE;

    if (generated_data) {
        is_linear = true;
        channels = 4;
        return IMAGE_DATA_TYPE_FLOAT4;
    }

	if(builtin_data) {
		bool is_float = false;
		if(builtin_image_info_cb) {
			int width, height, depth;
			builtin_image_info_cb(filename, builtin_data, is_float, width, height, depth, channels);
		}

		if(is_float) {
			is_linear = true;
			return (channels > 1) ? IMAGE_DATA_TYPE_FLOAT4 : IMAGE_DATA_TYPE_FLOAT;
		}
		else {
			return (channels > 1) ? IMAGE_DATA_TYPE_BYTE4 : IMAGE_DATA_TYPE_BYTE;
		}
	}

	/* Perform preliminary  checks, with meaningful logging. */
	if(!path_exists(filename)) {
		VLOG(1) << "File '" << filename << "' does not exist.";
		return IMAGE_DATA_TYPE_BYTE4;
	}
	if(path_is_directory(filename)) {
		VLOG(1) << "File '" << filename << "' is a directory, can't use as image.";
		return IMAGE_DATA_TYPE_BYTE4;
	}

	ImageInput *in = ImageInput::create(filename);

	if(in) {
		ImageSpec spec;

		if(in->open(filename, spec)) {
			/* check the main format, and channel formats;
			 * if any take up more than one byte, we'll need a float texture slot */
			if(spec.format.basesize() > channel_size) {
				channel_size = spec.format.basesize();
			}

			for(size_t channel = 0; channel < spec.channelformats.size(); channel++) {
				if(spec.channelformats[channel].basesize() > channel_size) {
					channel_size = spec.channelformats[channel].basesize();
				}
			}

			bool is_float = spec.format.is_floating_point();

			/* basic color space detection, not great but better than nothing
			 * before we do OpenColorIO integration */
			if(is_float) {
				string colorspace = spec.get_string_attribute("oiio:ColorSpace");

				is_linear = !(colorspace == "sRGB" ||
				              colorspace == "GammaCorrected" ||
				              (colorspace == "" &&
				                  (strcmp(in->format_name(), "png") == 0 ||
				                   strcmp(in->format_name(), "tiff") == 0 ||
				                   strcmp(in->format_name(), "dpx") == 0 ||
				                   strcmp(in->format_name(), "jpeg2000") == 0)));
			}
			else {
				is_linear = false;
			}

			channels = spec.nchannels;

			/* Default to float if we have no type that matches better. */
			type = (channels > 1) ? IMAGE_DATA_TYPE_FLOAT4 : IMAGE_DATA_TYPE_FLOAT;

			if(spec.format == TypeDesc::HALF) {
				type = (channels > 1) ? IMAGE_DATA_TYPE_HALF4 : IMAGE_DATA_TYPE_HALF;

			}
			else {
				if(channel_size == 1) {
					type = (channels > 1) ? IMAGE_DATA_TYPE_BYTE4 : IMAGE_DATA_TYPE_BYTE;
				}
				else if(spec.format == TypeDesc::UINT16) {
					type = (channels > 1) ? IMAGE_DATA_TYPE_USHORT4 : IMAGE_DATA_TYPE_USHORT;
				}
			}

			in->close();
		}

		delete in;
	}

	return type;
}

const string ImageManager::get_mip_map_path(const string& filename)
{
	if(!path_exists(filename)) {
		return "";
	}
	
	string::size_type idx = filename.rfind('.');
	if(idx != string::npos) {
		std::string extension = filename.substr(idx+1);
		if(extension == "tx") {
			return filename;
		}
	}
	
	string tx_name = filename.substr(0, idx) + ".tx";
	if(path_exists(tx_name)) {
		return tx_name;
	}

	return "";
}

/* The lower three bits of a device texture slot number indicate its type.
 * These functions convert the slot ids from ImageManager "images" ones
 * to device ones and vice versa.
 *
 * There are special cases for CUDA Fermi, since there we have only 90 image texture
 * slots available and shold keep the flattended numbers in the 0-89 range.
 */
int ImageManager::type_index_to_flattened_slot(int slot, ImageDataType type)
{
	if (cuda_fermi_limits) {
		if (type == IMAGE_DATA_TYPE_BYTE4) {
			return slot + 5;
		}
		else {
			return slot;
		}
	}

	return (slot << 3) | (type);
}

int ImageManager::flattened_slot_to_type_index(int flat_slot, ImageDataType *type)
{
	if (cuda_fermi_limits) {
		if (flat_slot >= 4) {
			*type = IMAGE_DATA_TYPE_BYTE4;
			return flat_slot - 5;
		}
		else {
			*type = IMAGE_DATA_TYPE_FLOAT4;
			return flat_slot;
		}
	}

	*type = static_cast<ImageDataType>(flat_slot & 0x7);
	return flat_slot >> 3;
}

string ImageManager::name_from_type(int type)
{
	if(type == IMAGE_DATA_TYPE_FLOAT4)
		return "float4";
	else if(type == IMAGE_DATA_TYPE_FLOAT)
		return "float";
	else if(type == IMAGE_DATA_TYPE_BYTE)
		return "byte";
	else if(type == IMAGE_DATA_TYPE_HALF4)
		return "half4";
	else if(type == IMAGE_DATA_TYPE_HALF)
		return "half";
	else if(type == IMAGE_DATA_TYPE_USHORT)
		return "ushort";
	else if(type == IMAGE_DATA_TYPE_USHORT4)
		return "ushort4";
	else
		return "byte4";
}

static bool image_equals(ImageManager::Image *image,
                         const string& filename,
                         void *builtin_data,
						 boost::shared_ptr<uint8_t> generated_data,
                         InterpolationType interpolation,
                         ExtensionType extension)
{
	return image->filename == filename &&
		   image->builtin_data == builtin_data &&
		   image->generated_data == generated_data &&
		   image->interpolation == interpolation &&
		   image->extension == extension;
}

int ImageManager::add_image(const string& filename,
                            void *builtin_data,
                            boost::shared_ptr<uint8_t> generated_data,
                            bool animated,
                            float frame,
                            bool& is_float,
                            bool& is_linear,
                            InterpolationType interpolation,
                            ExtensionType extension,
                            bool use_alpha,
                            bool srgb)
{
	Image *img;
	size_t slot;

	ImageDataType type = get_image_metadata(filename, builtin_data, generated_data, is_linear);

	thread_scoped_lock device_lock(device_mutex);

	/* Check whether it's a float texture. */
	is_float = (type == IMAGE_DATA_TYPE_FLOAT || type == IMAGE_DATA_TYPE_FLOAT4);

	/* No half on OpenCL, use available slots */
	if(type == IMAGE_DATA_TYPE_HALF4 && !has_half_images) {
		type = IMAGE_DATA_TYPE_FLOAT4;
	}
	else if(type == IMAGE_DATA_TYPE_HALF && !has_half_images) {
		type = IMAGE_DATA_TYPE_FLOAT;
	}

	if (type == IMAGE_DATA_TYPE_FLOAT && cuda_fermi_limits) {
		type = IMAGE_DATA_TYPE_FLOAT4;
	}
	else if (type == IMAGE_DATA_TYPE_BYTE && cuda_fermi_limits) {
		type = IMAGE_DATA_TYPE_BYTE4;
	}

	/* Fnd existing image. */
	for(slot = 0; slot < images[type].size(); slot++) {
		img = images[type][slot];
		if(img && image_equals(img,
		                       filename,
		                       builtin_data,
                               generated_data,
		                       interpolation,
		                       extension))
		{
			if(img->frame != frame) {
				img->frame = frame;
				img->need_load = true;
			}
			if(img->use_alpha != use_alpha) {
				img->use_alpha = use_alpha;
				img->need_load = true;
			}
			img->users++;
			return type_index_to_flattened_slot(slot, type);
		}
	}

	/* Find free slot. */
	for(slot = 0; slot < images[type].size(); slot++) {
		if(!images[type][slot])
			break;
	}

	/* Count if we're over the limit */
	if (cuda_fermi_limits) {
		if (tex_num_images[IMAGE_DATA_TYPE_BYTE4] == TEX_NUM_BYTE4_CUDA
			|| tex_num_images[IMAGE_DATA_TYPE_FLOAT4] == TEX_NUM_FLOAT4_CUDA)
		{
			printf("ImageManager::add_image: Reached %s image limit (%d), skipping '%s'\n",
				name_from_type(type).c_str(), tex_num_images[type], filename.c_str());
			return -1;
		}
	}
	else {
		/* Very unlikely, since max_num_images is insanely big. But better safe than sorry. */
		int tex_count = 0;
		for (int type = 0; type < IMAGE_DATA_NUM_TYPES; type++) {
			tex_count += tex_num_images[type];
		}
		if (tex_count > max_num_images) {
			printf("ImageManager::add_image: Reached image limit (%d), skipping '%s'\n",
				max_num_images, filename.c_str());
			return -1;
		}
	}
	
	if(slot == images[type].size()) {
		images[type].resize(images[type].size() + 1);
	}

	/* Add new image. */
	img = new Image();
	img->filename = filename;
	img->builtin_data = builtin_data;
    img->generated_data = generated_data;
	img->need_load = true;
	img->animated = animated;
	img->frame = frame;
	img->interpolation = interpolation;
	img->extension = extension;
	img->users = 1;
	img->use_alpha = use_alpha;
	img->srgb = srgb && (!is_linear);

	images[type][slot] = img;
	
	++tex_num_images[type];

	need_update = true;

	return type_index_to_flattened_slot(slot, type);
}

void ImageManager::remove_image(int flat_slot)
{
	ImageDataType type;
	int slot = flattened_slot_to_type_index(flat_slot, &type);

	Image *image = images[type][slot];
	assert(image && image->users >= 1);

	/* decrement user count */
	image->users--;

	/* don't remove immediately, rather do it all together later on. one of
	 * the reasons for this is that on shader changes we add and remove nodes
	 * that use them, but we do not want to reload the image all the time. */
	if(image->users == 0)
		need_update = true;
}

void ImageManager::remove_image(const string& filename,
                                void *builtin_data,
                                boost::shared_ptr<uint8_t> generated_data,
                                InterpolationType interpolation,
                                ExtensionType extension)
{
	size_t slot;

	for(int type = 0; type < IMAGE_DATA_NUM_TYPES; type++) {
		for(slot = 0; slot < images[type].size(); slot++) {
			if(images[type][slot] && image_equals(images[type][slot],
			                                      filename,
			                                      builtin_data,
                                                  generated_data,
			                                      interpolation,
			                                      extension))
			{
				remove_image(type_index_to_flattened_slot(slot, (ImageDataType)type));
				return;
			}
		}
	}
}

/* TODO(sergey): Deduplicate with the iteration above, but make it pretty,
 * without bunch of arguments passing around making code readability even
 * more cluttered.
 */
void ImageManager::tag_reload_image(const string& filename,
                                    void *builtin_data,
									boost::shared_ptr<uint8_t> generated_data,
                                    InterpolationType interpolation,
                                    ExtensionType extension)
{
	for(size_t type = 0; type < IMAGE_DATA_NUM_TYPES; type++) {
		for(size_t slot = 0; slot < images[type].size(); slot++) {
			if(images[type][slot] && image_equals(images[type][slot],
			                                      filename,
			                                      builtin_data,
                                                  generated_data,
			                                      interpolation,
			                                      extension))
			{
				images[type][slot]->need_load = true;
				break;
			}
		}
	}
}

bool ImageManager::file_load_image_generic(Image *img, ImageInput **in, int &width, int &height, int &depth, int &components)
{
	if(img->filename == "")
		return false;

	if(!img->builtin_data && !img->generated_data) {
		/* NOTE: Error logging is done in meta data acquisition. */
		if(!path_exists(img->filename) || path_is_directory(img->filename)) {
			return false;
		}

		/* load image from file through OIIO */
		*in = ImageInput::create(img->filename);

		if(!*in)
			return false;

		ImageSpec spec = ImageSpec();
		ImageSpec config = ImageSpec();

		if(img->use_alpha == false)
			config.attribute("oiio:UnassociatedAlpha", 1);

		if(!(*in)->open(img->filename, spec, config)) {
			delete *in;
			*in = NULL;
			return false;
		}

		width = spec.width;
		height = spec.height;
		depth = spec.depth;
		components = spec.nchannels;
	}
	else {

        if (img->generated_data) {
            InternalImageHeader *header = (InternalImageHeader*) img->generated_data.get();
            width = header->width;
            height = header->height;
            depth = 1;
            components = 4;

        } else {
            /* load image using builtin images callbacks */
            if(!builtin_image_info_cb || !builtin_image_pixels_cb)
                return false;

            bool is_float;
            builtin_image_info_cb(img->filename, img->builtin_data, is_float, width, height, depth, components);
        }
	}

	/* we only handle certain number of components */
	if(!(components >= 1 && components <= 4)) {
		if(*in) {
			(*in)->close();
			delete *in;
			*in = NULL;
		}

		return false;
	}

	return true;
}

template<TypeDesc::BASETYPE FileFormat,
         typename StorageType,
         typename DeviceType>
bool ImageManager::file_load_image(Image *img,
                                   ImageDataType type,
                                   int texture_limit,
                                   device_vector<DeviceType>& tex_img)
{
	const StorageType alpha_one = (FileFormat == TypeDesc::UINT8)? 255 : 1;
	ImageInput *in = NULL;
	int width, height, depth, components;
	if(!file_load_image_generic(img, &in, width, height, depth, components)) {
		return false;
	}
	/* Read RGBA pixels. */
	vector<StorageType> pixels_storage;
	StorageType *pixels;
	const size_t max_size = max(max(width, height), depth);
	if(texture_limit > 0 && max_size > texture_limit) {
		pixels_storage.resize(((size_t)width)*height*depth*4);
		pixels = &pixels_storage[0];
	}
	else {
		pixels = (StorageType*)tex_img.resize(width, height, depth);
	}
	bool cmyk = false;
	const size_t num_pixels = ((size_t)width) * height * depth;
	if(in) {
		StorageType *readpixels = pixels;
		vector<StorageType> tmppixels;
		if(components > 4) {
			tmppixels.resize(((size_t)width)*height*components);
			readpixels = &tmppixels[0];
		}
		if(depth <= 1) {
			size_t scanlinesize = ((size_t)width)*components*sizeof(StorageType);
			in->read_image(FileFormat,
			               (uchar*)readpixels + (height-1)*scanlinesize,
			               AutoStride,
			               -scanlinesize,
			               AutoStride);
		}
		else {
			in->read_image(FileFormat, (uchar*)readpixels);
		}
		if(components > 4) {
			size_t dimensions = ((size_t)width)*height;
			for(size_t i = dimensions-1, pixel = 0; pixel < dimensions; pixel++, i--) {
				pixels[i*4+3] = tmppixels[i*components+3];
				pixels[i*4+2] = tmppixels[i*components+2];
				pixels[i*4+1] = tmppixels[i*components+1];
				pixels[i*4+0] = tmppixels[i*components+0];
			}
			tmppixels.clear();
		}
		cmyk = strcmp(in->format_name(), "jpeg") == 0 && components == 4;
		in->close();
		delete in;
	}
	else {
        if (img->generated_data) {
            InternalImageHeader *header = (InternalImageHeader*) img->generated_data.get();
            ::memcpy(&pixels[0], (uint8_t*) img->generated_data.get() + sizeof(InternalImageHeader), header->width * header->height * sizeof(float4));

        } else if(FileFormat == TypeDesc::FLOAT) {
			builtin_image_float_pixels_cb(img->filename,
			                              img->builtin_data,
			                              (float*)&pixels[0],
			                              num_pixels * components);
		}
		else if(FileFormat == TypeDesc::UINT8) {
			builtin_image_pixels_cb(img->filename,
			                        img->builtin_data,
			                        (uchar*)&pixels[0],
			                        num_pixels * components);
		}
		else {
			/* TODO(dingto): Support half for ImBuf. */
		}
	}
	/* Check if we actually have a float4 slot, in case components == 1,
	 * but device doesn't support single channel textures.
	 */
	bool is_rgba = (type == IMAGE_DATA_TYPE_FLOAT4 ||
	                type == IMAGE_DATA_TYPE_HALF4 ||
	                type == IMAGE_DATA_TYPE_BYTE4 ||
					type == IMAGE_DATA_TYPE_USHORT4);
	if(is_rgba) {
		if(cmyk) {
			/* CMYK */
			for(size_t i = num_pixels-1, pixel = 0; pixel < num_pixels; pixel++, i--) {
				pixels[i*4+2] = (pixels[i*4+2]*pixels[i*4+3])/255;
				pixels[i*4+1] = (pixels[i*4+1]*pixels[i*4+3])/255;
				pixels[i*4+0] = (pixels[i*4+0]*pixels[i*4+3])/255;
				pixels[i*4+3] = alpha_one;
			}
		}
		else if(components == 2) {
			/* grayscale + alpha */
			for(size_t i = num_pixels-1, pixel = 0; pixel < num_pixels; pixel++, i--) {
				pixels[i*4+3] = pixels[i*2+1];
				pixels[i*4+2] = pixels[i*2+0];
				pixels[i*4+1] = pixels[i*2+0];
				pixels[i*4+0] = pixels[i*2+0];
			}
		}
		else if(components == 3) {
			/* RGB */
			for(size_t i = num_pixels-1, pixel = 0; pixel < num_pixels; pixel++, i--) {
				pixels[i*4+3] = alpha_one;
				pixels[i*4+2] = pixels[i*3+2];
				pixels[i*4+1] = pixels[i*3+1];
				pixels[i*4+0] = pixels[i*3+0];
			}
		}
		else if(components == 1) {
			/* grayscale */
			for(size_t i = num_pixels-1, pixel = 0; pixel < num_pixels; pixel++, i--) {
				pixels[i*4+3] = alpha_one;
				pixels[i*4+2] = pixels[i];
				pixels[i*4+1] = pixels[i];
				pixels[i*4+0] = pixels[i];
			}
		}
		if(img->use_alpha == false) {
			for(size_t i = num_pixels-1, pixel = 0; pixel < num_pixels; pixel++, i--) {
				pixels[i*4+3] = alpha_one;
			}
		}
	}
	if(pixels_storage.size() > 0) {
		float scale_factor = 1.0f;
		while(max_size * scale_factor > texture_limit) {
			scale_factor *= 0.5f;
		}
		VLOG(1) << "Scaling image " << img->filename
		        << " by a factor of " << scale_factor << ".";
		vector<StorageType> scaled_pixels;
		size_t scaled_width, scaled_height, scaled_depth;
		util_image_resize_pixels(pixels_storage,
		                         width, height, depth,
		                         is_rgba ? 4 : 1,
		                         scale_factor,
		                         &scaled_pixels,
		                         &scaled_width, &scaled_height, &scaled_depth);
		StorageType *texture_pixels = (StorageType*)tex_img.resize(scaled_width,
		                                                           scaled_height,
		                                                           scaled_depth);
		memcpy(texture_pixels,
		       &scaled_pixels[0],
		       scaled_pixels.size() * sizeof(StorageType));
	}
	return true;
}

void ImageManager::device_load_image(Device *device,
                                     DeviceScene *dscene,
                                     Scene *scene,
                                     ImageDataType type,
                                     int slot,
                                     Progress *progress)
{
	if(progress->get_cancel())
		return;

	Image *img = images[type][slot];
	if(!img) {
		return;
	}

	if(oiio_texture_system && !img->builtin_data) {
		/* Get or generate a mip mapped tile image file.
		 * If we have a mip map, assume it's linear, not sRGB. */
		bool have_mip = get_tx(img, progress, scene->params.texture.auto_convert);

		/* When using OIIO directly from SVM, store the TextureHandle
		 * in an array for quicker lookup at shading time */
		OIIOGlobals *oiio = (OIIOGlobals*)device->oiio_memory();
		if(oiio) {
			thread_scoped_lock lock(oiio->tex_paths_mutex);
			int flat_slot = type_index_to_flattened_slot(slot, type);
			if (oiio->textures.size() <= flat_slot) {
				oiio->textures.resize(flat_slot+1);
			}
			OIIO::TextureSystem *tex_sys = (OIIO::TextureSystem*)oiio_texture_system;
			OIIO::TextureSystem::TextureHandle *handle = tex_sys->get_texture_handle(OIIO::ustring(img->filename.c_str()));
			if(tex_sys->good(handle)) {
				oiio->textures[flat_slot].handle = handle;
				switch(img->interpolation) {
					case INTERPOLATION_SMART:
						oiio->textures[flat_slot].interpolation = OIIO::TextureOpt::InterpSmartBicubic;
						break;
					case INTERPOLATION_CUBIC:
						oiio->textures[flat_slot].interpolation = OIIO::TextureOpt::InterpBicubic;
						break;
					case INTERPOLATION_LINEAR:
						oiio->textures[flat_slot].interpolation = OIIO::TextureOpt::InterpBilinear;
						break;
					case INTERPOLATION_NONE:
					case INTERPOLATION_CLOSEST:
					default:
						oiio->textures[flat_slot].interpolation = OIIO::TextureOpt::InterpClosest;
						break;
				}
				switch(img->extension) {
					case EXTENSION_CLIP:
						oiio->textures[flat_slot].extension = OIIO::TextureOpt::WrapBlack;
						break;
					case EXTENSION_EXTEND:
						oiio->textures[flat_slot].extension = OIIO::TextureOpt::WrapClamp;
						break;
					case EXTENSION_REPEAT:
					default:
						oiio->textures[flat_slot].extension = OIIO::TextureOpt::WrapPeriodic;
						break;
				}
				oiio->textures[flat_slot].is_linear = have_mip;
			} else {
				oiio->textures[flat_slot].handle = NULL;
			}
		}
		img->need_load = false;
		return;
	}

	string filename = path_filename(img->filename);
	progress->set_status("Updating Images", "Loading " + filename);

	const int texture_limit = scene->params.texture_limit;

	/* Slot assignment */
	int flat_slot = type_index_to_flattened_slot(slot, type);

	string name = string_printf("__tex_image_%s_%03d", name_from_type(type).c_str(), flat_slot);

	if(type == IMAGE_DATA_TYPE_FLOAT4) {
		if(dscene->tex_float4_image[slot] == NULL)
			dscene->tex_float4_image[slot] = new device_vector<float4>();
		device_vector<float4>& tex_img = *dscene->tex_float4_image[slot];

		if(tex_img.device_pointer) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_free(tex_img);
		}

		if(!file_load_image<TypeDesc::FLOAT, float>(img,
		                                            type,
		                                            texture_limit,
		                                            tex_img))
		{
			/* on failure to load, we set a 1x1 pixels pink image */
			float *pixels = (float*)tex_img.resize(1, 1);

			pixels[0] = TEX_IMAGE_MISSING_R;
			pixels[1] = TEX_IMAGE_MISSING_G;
			pixels[2] = TEX_IMAGE_MISSING_B;
			pixels[3] = TEX_IMAGE_MISSING_A;
		}

		if(!pack_images) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_alloc(name.c_str(),
			                  tex_img,
			                  img->interpolation,
			                  img->extension);
		}
	}
	else if(type == IMAGE_DATA_TYPE_FLOAT) {
		if (slot >= dscene->tex_float_image.size()) {
			return;
		}
		if(dscene->tex_float_image[slot] == NULL)
			dscene->tex_float_image[slot] = new device_vector<float>();
		device_vector<float>& tex_img = *dscene->tex_float_image[slot];

		if(tex_img.device_pointer) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_free(tex_img);
		}

		if(!file_load_image<TypeDesc::FLOAT, float>(img,
		                                            type,
		                                            texture_limit,
		                                            tex_img))
		{
			/* on failure to load, we set a 1x1 pixels pink image */
			float *pixels = (float*)tex_img.resize(1, 1);

			pixels[0] = TEX_IMAGE_MISSING_R;
		}

		if(!pack_images) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_alloc(name.c_str(),
			                  tex_img,
			                  img->interpolation,
			                  img->extension);
		}
	}
	else if(type == IMAGE_DATA_TYPE_BYTE4) {
		if (slot >= dscene->tex_byte4_image.size()) {
			return;
		}
		if(dscene->tex_byte4_image[slot] == NULL)
			dscene->tex_byte4_image[slot] = new device_vector<uchar4>();
		device_vector<uchar4>& tex_img = *dscene->tex_byte4_image[slot];

		if(tex_img.device_pointer) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_free(tex_img);
		}

		if(!file_load_image<TypeDesc::UINT8, uchar>(img,
		                                            type,
		                                            texture_limit,
		                                            tex_img))
		{
			/* on failure to load, we set a 1x1 pixels pink image */
			uchar *pixels = (uchar*)tex_img.resize(1, 1);

			pixels[0] = (TEX_IMAGE_MISSING_R * 255);
			pixels[1] = (TEX_IMAGE_MISSING_G * 255);
			pixels[2] = (TEX_IMAGE_MISSING_B * 255);
			pixels[3] = (TEX_IMAGE_MISSING_A * 255);
		}

		if(!pack_images) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_alloc(name.c_str(),
			                  tex_img,
			                  img->interpolation,
			                  img->extension);
		}
	}
	else if(type == IMAGE_DATA_TYPE_BYTE) {
		if (slot >= dscene->tex_byte_image.size()) {
			return;
		}
		if(dscene->tex_byte_image[slot] == NULL)
			dscene->tex_byte_image[slot] = new device_vector<uchar>();
		device_vector<uchar>& tex_img = *dscene->tex_byte_image[slot];

		if(tex_img.device_pointer) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_free(tex_img);
		}

		if(!file_load_image<TypeDesc::UINT8, uchar>(img,
		                                            type,
		                                            texture_limit,
		                                            tex_img)) {
			/* on failure to load, we set a 1x1 pixels pink image */
			uchar *pixels = (uchar*)tex_img.resize(1, 1);

			pixels[0] = (TEX_IMAGE_MISSING_R * 255);
		}

		if(!pack_images) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_alloc(name.c_str(),
			                  tex_img,
			                  img->interpolation,
			                  img->extension);
		}
	}
	else if(type == IMAGE_DATA_TYPE_HALF4){
		if (slot >= dscene->tex_half4_image.size()) {
			return;
		}
		if(dscene->tex_half4_image[slot] == NULL)
			dscene->tex_half4_image[slot] = new device_vector<half4>();
		device_vector<half4>& tex_img = *dscene->tex_half4_image[slot];

		if(tex_img.device_pointer) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_free(tex_img);
		}

		if(!file_load_image<TypeDesc::HALF, half>(img,
		                                          type,
		                                          texture_limit,
		                                          tex_img)) {
			/* on failure to load, we set a 1x1 pixels pink image */
			half *pixels = (half*)tex_img.resize(1, 1);

			pixels[0] = TEX_IMAGE_MISSING_R;
			pixels[1] = TEX_IMAGE_MISSING_G;
			pixels[2] = TEX_IMAGE_MISSING_B;
			pixels[3] = TEX_IMAGE_MISSING_A;
		}

		if(!pack_images) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_alloc(name.c_str(),
			                  tex_img,
			                  img->interpolation,
			                  img->extension);
		}
	}
	else if(type == IMAGE_DATA_TYPE_HALF){
		if (slot >= dscene->tex_half_image.size()) {
			return;
		}
		if(dscene->tex_half_image[slot] == NULL)
			dscene->tex_half_image[slot] = new device_vector<half>();
		device_vector<half>& tex_img = *dscene->tex_half_image[slot];

		if(tex_img.device_pointer) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_free(tex_img);
		}

		if(!file_load_image<TypeDesc::HALF, half>(img,
		                                          type,
		                                          texture_limit,
		                                          tex_img)) {
			/* on failure to load, we set a 1x1 pixels pink image */
			half *pixels = (half*)tex_img.resize(1, 1);

			pixels[0] = TEX_IMAGE_MISSING_R;
		}

		if(!pack_images) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_alloc(name.c_str(),
			                  tex_img,
			                  img->interpolation,
			                  img->extension);
		}
	}
	else if(type == IMAGE_DATA_TYPE_USHORT4){
		if (slot >= dscene->tex_ushort4_image.size()) {
			return;
		}
		if(dscene->tex_ushort4_image[slot] == NULL)
			dscene->tex_ushort4_image[slot] = new device_vector<ushort4>();
		device_vector<ushort4>& tex_img = *dscene->tex_ushort4_image[slot];

		if(tex_img.device_pointer) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_free(tex_img);
		}

		if(!file_load_image<TypeDesc::USHORT, half>(img,
												  type,
												  texture_limit,
												  tex_img)) {
			/* on failure to load, we set a 1x1 pixels pink image */
			ushort *pixels = (ushort*)tex_img.resize(1, 1);

			pixels[0] = TEX_IMAGE_MISSING_R * 65535;
			pixels[1] = TEX_IMAGE_MISSING_G * 65535;
			pixels[2] = TEX_IMAGE_MISSING_B * 65535;
			pixels[3] = TEX_IMAGE_MISSING_A * 65535;
		}

		if(!pack_images) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_alloc(name.c_str(),
							  tex_img,
							  img->interpolation,
							  img->extension);
		}
	}
	else if(type == IMAGE_DATA_TYPE_USHORT){
		if (slot >= dscene->tex_ushort_image.size()) {
			return;
		}
		if(dscene->tex_ushort_image[slot] == NULL)
			dscene->tex_ushort_image[slot] = new device_vector<uint16_t>();
		device_vector<uint16_t>& tex_img = *dscene->tex_ushort_image[slot];

		if(tex_img.device_pointer) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_free(tex_img);
		}

		if(!file_load_image<TypeDesc::USHORT, half>(img,
												  type,
												  texture_limit,
												  tex_img)) {
			/* on failure to load, we set a 1x1 pixels pink image */
			uint16_t *pixels = (uint16_t*)tex_img.resize(1, 1);

			pixels[0] = TEX_IMAGE_MISSING_R * 65535;
		}

		if(!pack_images) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_alloc(name.c_str(),
							  tex_img,
							  img->interpolation,
							  img->extension);
		}
	}
	img->need_load = false;
}

void ImageManager::device_free_image(Device *device, DeviceScene *dscene, ImageDataType type, int slot)
{
	Image *img = images[type][slot];

	if(img) {
		if(oiio_texture_system && !img->builtin_data) {
			ustring filename(images[type][slot]->filename);
		//	((OIIO::TextureSystem*)oiio_texture_system)->invalidate(filename);
		}
		else {
			device_memory *tex_img = NULL;
			switch(type) {
				case IMAGE_DATA_TYPE_FLOAT4:
					if(slot >= dscene->tex_float4_image.size()) {
						break;
					}
					tex_img = dscene->tex_float4_image[slot];
					dscene->tex_float4_image[slot] = NULL;
					break;
				case IMAGE_DATA_TYPE_FLOAT:
					if(slot >= dscene->tex_float_image.size()) {
						break;
					}
					tex_img = dscene->tex_float_image[slot];
					dscene->tex_float_image[slot] = NULL;
					break;
				case IMAGE_DATA_TYPE_BYTE:
					if(slot >= dscene->tex_byte_image.size()) {
						break;
					}
					tex_img = dscene->tex_byte_image[slot];
					dscene->tex_byte_image[slot]= NULL;
					break;
				case IMAGE_DATA_TYPE_BYTE4:
					if(slot >= dscene->tex_byte4_image.size()) {
						break;
					}
					tex_img = dscene->tex_byte4_image[slot];
					dscene->tex_byte4_image[slot] = NULL;
					break;
				case IMAGE_DATA_TYPE_HALF:
					if(slot >= dscene->tex_half_image.size()) {
						break;
					}
					tex_img = dscene->tex_half_image[slot];
					dscene->tex_half_image[slot]= NULL;
					break;
				case IMAGE_DATA_TYPE_HALF4:
					if(slot >= dscene->tex_half4_image.size()) {
						break;
					}
					tex_img = dscene->tex_half4_image[slot];
					dscene->tex_half4_image[slot]= NULL;
					break;
				case IMAGE_DATA_TYPE_USHORT:
					if(slot >= dscene->tex_ushort_image.size()) {
						break;
					}
					tex_img = dscene->tex_ushort_image[slot];
					dscene->tex_ushort_image[slot]= NULL;
					break;
				case IMAGE_DATA_TYPE_USHORT4:
					if(slot >= dscene->tex_ushort4_image.size()) {
						break;
					}
					tex_img = dscene->tex_ushort4_image[slot];
					dscene->tex_ushort4_image[slot]= NULL;
					break;
				default:
					assert(0);
					tex_img = NULL;
			}
			if(tex_img) {
				if(tex_img->device_pointer) {
					thread_scoped_lock device_lock(device_mutex);
					device->tex_free(*tex_img);
				}

				delete tex_img;
			}
		}

		delete images[type][slot];
		images[type][slot] = NULL;
		--tex_num_images[type];
	}
}

void ImageManager::device_prepare_update(DeviceScene *dscene)
{
	for(int type = 0; type < IMAGE_DATA_NUM_TYPES; type++) {
		switch(type) {
			case IMAGE_DATA_TYPE_BYTE4:
				if (dscene->tex_byte4_image.size() <= tex_num_images[IMAGE_DATA_TYPE_BYTE4])
					dscene->tex_byte4_image.resize(tex_num_images[IMAGE_DATA_TYPE_BYTE4]);
				break;
			case IMAGE_DATA_TYPE_FLOAT4:
				if (dscene->tex_float4_image.size() <= tex_num_images[IMAGE_DATA_TYPE_FLOAT4])
					dscene->tex_float4_image.resize(tex_num_images[IMAGE_DATA_TYPE_FLOAT4]);
				break;
			case IMAGE_DATA_TYPE_BYTE:
				if (dscene->tex_byte_image.size() <= tex_num_images[IMAGE_DATA_TYPE_BYTE])
					dscene->tex_byte_image.resize(tex_num_images[IMAGE_DATA_TYPE_BYTE]);
				break;
			case IMAGE_DATA_TYPE_FLOAT:
				if (dscene->tex_float_image.size() <= tex_num_images[IMAGE_DATA_TYPE_FLOAT])
					dscene->tex_float_image.resize(tex_num_images[IMAGE_DATA_TYPE_FLOAT]);
				break;
			case IMAGE_DATA_TYPE_HALF4:
				if (dscene->tex_half4_image.size() <= tex_num_images[IMAGE_DATA_TYPE_HALF4])
					dscene->tex_half4_image.resize(tex_num_images[IMAGE_DATA_TYPE_HALF4]);
				break;
			case IMAGE_DATA_TYPE_HALF:
				if (dscene->tex_half_image.size() <= tex_num_images[IMAGE_DATA_TYPE_HALF])
					dscene->tex_half_image.resize(tex_num_images[IMAGE_DATA_TYPE_HALF]);
				break;
			case IMAGE_DATA_TYPE_USHORT4:
				if (dscene->tex_ushort4_image.size() <= tex_num_images[IMAGE_DATA_TYPE_USHORT4])
					dscene->tex_ushort4_image.resize(tex_num_images[IMAGE_DATA_TYPE_USHORT4]);
				break;
			case IMAGE_DATA_TYPE_USHORT:
				if (dscene->tex_ushort_image.size() <= tex_num_images[IMAGE_DATA_TYPE_USHORT])
					dscene->tex_ushort_image.resize(tex_num_images[IMAGE_DATA_TYPE_USHORT]);
				break;
			default:
				assert(0);
		}
	}
}

void ImageManager::device_update(Device *device,
								 DeviceScene *dscene,
								 Scene *scene,
								 Progress& progress)
{
	if(!need_update) {
		return;
	}

	/* Make sure arrays are proper size. */
	device_prepare_update(dscene);

	TaskPool pool;
	for(int type = 0; type < IMAGE_DATA_NUM_TYPES; type++) {
		for(size_t slot = 0; slot < images[type].size(); slot++) {
			if(!images[type][slot])
				continue;

			if(images[type][slot]->users == 0) {
				device_free_image(device, dscene, (ImageDataType)type, slot);
			}
			else if(images[type][slot]->need_load) {
				pool.push(function_bind(&ImageManager::device_load_image,
										this,
										device,
										dscene,
										scene,
										(ImageDataType)type,
										slot,
										&progress));
			}
		}
	}

	pool.wait_work();

	if(pack_images)
		device_pack_images(device, dscene, progress);

	need_update = false;
}

void ImageManager::device_update_slot(Device *device,
                                      DeviceScene *dscene,
                                      Scene *scene,
                                      int flat_slot,
                                      Progress *progress)
{
	ImageDataType type;
	int slot = flattened_slot_to_type_index(flat_slot, &type);

	Image *image = images[type][slot];
	assert(image != NULL);

	if(image->users == 0) {
		device_free_image(device, dscene, type, slot);
	}
	else if(image->need_load) {
		device_load_image(device,
						  dscene,
						  scene,
						  type,
						  slot,
						  progress);
	}
}

uint8_t ImageManager::pack_image_options(ImageDataType type, size_t slot)
{
	uint8_t options = 0;

	/* Image Options are packed into one uint:
	 * bit 0 -> Interpolation
	 * bit 1 + 2  + 3-> Extension */
	if(images[type][slot]->interpolation == INTERPOLATION_CLOSEST)
		options |= (1 << 0);

	if(images[type][slot]->extension == EXTENSION_REPEAT)
		options |= (1 << 1);
	else if(images[type][slot]->extension == EXTENSION_EXTEND)
		options |= (1 << 2);
	else /* EXTENSION_CLIP */
		options |= (1 << 3);

	return options;
}

void ImageManager::device_pack_images(Device *device,
                                      DeviceScene *dscene,
                                      Progress& /*progess*/)
{
	/* For OpenCL, we pack all image textures into a single large texture, and
	 * do our own interpolation in the kernel. */
	size_t size = 0, offset = 0;
	ImageDataType type;

	int info_size = tex_num_images[IMAGE_DATA_TYPE_FLOAT4] + tex_num_images[IMAGE_DATA_TYPE_BYTE4]
	                + tex_num_images[IMAGE_DATA_TYPE_FLOAT] + tex_num_images[IMAGE_DATA_TYPE_BYTE];
	uint4 *info = dscene->tex_image_packed_info.resize(info_size*2);

	/* Byte4 Textures*/
	type = IMAGE_DATA_TYPE_BYTE4;

	for(size_t slot = 0; slot < images[type].size(); slot++) {
		if(!images[type][slot])
			continue;

		device_vector<uchar4>& tex_img = *dscene->tex_byte4_image[slot];
		size += tex_img.size();
	}

	uchar4 *pixels_byte4 = dscene->tex_image_byte4_packed.resize(size);

	for(size_t slot = 0; slot < images[type].size(); slot++) {
		if(!images[type][slot])
			continue;

		device_vector<uchar4>& tex_img = *dscene->tex_byte4_image[slot];

		uint8_t options = pack_image_options(type, slot);

		int index = type_index_to_flattened_slot(slot, type) * 2;
		info[index] = make_uint4(tex_img.data_width, tex_img.data_height, offset, options);
		info[index+1] = make_uint4(tex_img.data_depth, 0, 0, 0);

		memcpy(pixels_byte4+offset, (void*)tex_img.data_pointer, tex_img.memory_size());
		offset += tex_img.size();
	}

	/* Float4 Textures*/
	type = IMAGE_DATA_TYPE_FLOAT4;
	size = 0, offset = 0;

	for(size_t slot = 0; slot < images[type].size(); slot++) {
		if(!images[type][slot])
			continue;

		device_vector<float4>& tex_img = *dscene->tex_float4_image[slot];
		size += tex_img.size();
	}

	float4 *pixels_float4 = dscene->tex_image_float4_packed.resize(size);

	for(size_t slot = 0; slot < images[type].size(); slot++) {
		if(!images[type][slot])
			continue;

		device_vector<float4>& tex_img = *dscene->tex_float4_image[slot];

		/* todo: support 3D textures, only CPU for now */

		uint8_t options = pack_image_options(type, slot);

		int index = type_index_to_flattened_slot(slot, type) * 2;
		info[index] = make_uint4(tex_img.data_width, tex_img.data_height, offset, options);
		info[index+1] = make_uint4(tex_img.data_depth, 0, 0, 0);

		memcpy(pixels_float4+offset, (void*)tex_img.data_pointer, tex_img.memory_size());
		offset += tex_img.size();
	}

	/* Byte Textures*/
	type = IMAGE_DATA_TYPE_BYTE;
	size = 0, offset = 0;

	for(size_t slot = 0; slot < images[type].size(); slot++) {
		if(!images[type][slot])
			continue;

		device_vector<uchar>& tex_img = *dscene->tex_byte_image[slot];
		size += tex_img.size();
	}

	uchar *pixels_byte = dscene->tex_image_byte_packed.resize(size);

	for(size_t slot = 0; slot < images[type].size(); slot++) {
		if(!images[type][slot])
			continue;

		device_vector<uchar>& tex_img = *dscene->tex_byte_image[slot];

		uint8_t options = pack_image_options(type, slot);

		int index = type_index_to_flattened_slot(slot, type) * 2;
		info[index] = make_uint4(tex_img.data_width, tex_img.data_height, offset, options);
		info[index+1] = make_uint4(tex_img.data_depth, 0, 0, 0);

		memcpy(pixels_byte+offset, (void*)tex_img.data_pointer, tex_img.memory_size());
		offset += tex_img.size();
	}

	/* Float Textures*/
	type = IMAGE_DATA_TYPE_FLOAT;
	size = 0, offset = 0;

	for(size_t slot = 0; slot < images[type].size(); slot++) {
		if(!images[type][slot])
			continue;

		device_vector<float>& tex_img = *dscene->tex_float_image[slot];
		size += tex_img.size();
	}

	float *pixels_float = dscene->tex_image_float_packed.resize(size);

	for(size_t slot = 0; slot < images[type].size(); slot++) {
		if(!images[type][slot])
			continue;

		device_vector<float>& tex_img = *dscene->tex_float_image[slot];

		/* todo: support 3D textures, only CPU for now */

		uint8_t options = pack_image_options(type, slot);

		int index = type_index_to_flattened_slot(slot, type) * 2;
		info[index] = make_uint4(tex_img.data_width, tex_img.data_height, offset, options);
		info[index+1] = make_uint4(tex_img.data_depth, 0, 0, 0);

		memcpy(pixels_float+offset, (void*)tex_img.data_pointer, tex_img.memory_size());
		offset += tex_img.size();
	}

	if(dscene->tex_image_byte4_packed.size()) {
		if(dscene->tex_image_byte4_packed.device_pointer) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_free(dscene->tex_image_byte4_packed);
		}
		device->tex_alloc("__tex_image_byte4_packed", dscene->tex_image_byte4_packed);
	}
	if(dscene->tex_image_float4_packed.size()) {
		if(dscene->tex_image_float4_packed.device_pointer) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_free(dscene->tex_image_float4_packed);
		}
		device->tex_alloc("__tex_image_float4_packed", dscene->tex_image_float4_packed);
	}
	if(dscene->tex_image_byte_packed.size()) {
		if(dscene->tex_image_byte_packed.device_pointer) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_free(dscene->tex_image_byte_packed);
		}
		device->tex_alloc("__tex_image_byte_packed", dscene->tex_image_byte_packed);
	}
	if(dscene->tex_image_float_packed.size()) {
		if(dscene->tex_image_float_packed.device_pointer) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_free(dscene->tex_image_float_packed);
		}
		device->tex_alloc("__tex_image_float_packed", dscene->tex_image_float_packed);
	}
	if(dscene->tex_image_packed_info.size()) {
		if(dscene->tex_image_packed_info.device_pointer) {
			thread_scoped_lock device_lock(device_mutex);
			device->tex_free(dscene->tex_image_packed_info);
		}
		device->tex_alloc("__tex_image_packed_info", dscene->tex_image_packed_info);
	}
}

void ImageManager::device_free_builtin(Device *device, DeviceScene *dscene)
{
	for(int type = 0; type < IMAGE_DATA_NUM_TYPES; type++) {
		for(size_t slot = 0; slot < images[type].size(); slot++) {
			if(images[type][slot] && images[type][slot]->builtin_data)
				device_free_image(device, dscene, (ImageDataType)type, slot);
		}
	}
}

void ImageManager::device_free(Device *device, DeviceScene *dscene)
{
	for(int type = 0; type < IMAGE_DATA_NUM_TYPES; type++) {
		for(size_t slot = 0; slot < images[type].size(); slot++) {
			device_free_image(device, dscene, (ImageDataType)type, slot);
		}
		images[type].clear();
	}
	
	dscene->tex_byte4_image.clear();
	dscene->tex_byte_image.clear();
	dscene->tex_float4_image.clear();
	dscene->tex_float_image.clear();
	dscene->tex_half4_image.clear();
	dscene->tex_half_image.clear();

	device->tex_free(dscene->tex_image_byte4_packed);
	device->tex_free(dscene->tex_image_float4_packed);
	device->tex_free(dscene->tex_image_byte_packed);
	device->tex_free(dscene->tex_image_float_packed);
	device->tex_free(dscene->tex_image_packed_info);

	dscene->tex_image_byte4_packed.clear();
	dscene->tex_image_float4_packed.clear();
	dscene->tex_image_byte_packed.clear();
	dscene->tex_image_float_packed.clear();
	dscene->tex_image_packed_info.clear();
}

bool ImageManager::make_tx(const string &filename, const string &outputfilename, bool srgb)
{
	ImageSpec config;
	config.attribute("maketx:filtername", "lanczos3");
	config.attribute("maketx:opaque_detect", 1);
	config.attribute("maketx:highlightcomp", 1);
	config.attribute("maketx:updatemode", 1);
	config.attribute("maketx:oiio_options", 1);
	config.attribute("maketx:updatemode", 1);
	/* Convert textures to linear color space before mip mapping. */
	if(srgb) {
		config.attribute("maketx:incolorspace", "sRGB");
		config.attribute("maketx:outcolorspace", "linear");
	}

	return ImageBufAlgo::make_texture(ImageBufAlgo::MakeTxTexture, filename, outputfilename, config);
}

bool ImageManager::get_tx(Image *image, Progress *progress, bool auto_convert)
{
	if(!path_exists(image->filename)) {
		return false;
	}
	
	string::size_type idx = image->filename.rfind('.');
	if(idx != string::npos) {
		std::string extension = image->filename.substr(idx+1);
		if(extension == "tx") {
			return true;
		}
	}
	
	string tx_name = image->filename.substr(0, idx) + ".tx";
	if(path_exists(tx_name)) {
		image->filename = tx_name;
		return true;
	}
	
	if(auto_convert) {
		progress->set_status("Updating Images", "Converting " + image->filename);
	
		bool ok = make_tx(image->filename, tx_name, image->srgb);
		if(ok) {
			image->filename = tx_name;
		return true;
		}
	}
	return false;
}

CCL_NAMESPACE_END

