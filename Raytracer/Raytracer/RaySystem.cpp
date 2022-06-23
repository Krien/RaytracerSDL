#include "precomp.h"
#include "Shape.h"
#include <limits.h>
#include "xmmintrin.h"
#define SIZE SCREEN_WIDTH * SCREEN_HEIGHT

RaySystem::RaySystem(Screen* screen)
{
	this->width = screen->getWidth();
	this->height = screen->getHeight();
}

void RaySystem::init(Scene* scene, Camera* camera) {
	this->scene = scene;
	this->camera = camera;

	shapes = scene->objects;
	shapeSize = shapes.size();
	lights = scene->lights;
	lightSize = lights.size();

	Vec3Df startDir = camera->getRelTopLeft();
	Vec3Df camPos = camera->position;
	xOffset = (float)SCREEN_DIMENSION * 2 / (width - 1);
	yOffset = (float)SCREEN_DIMENSION * 2 / (height - 1);

	startX = _mm256_set1_ps(startDir.get_x());
	startY = _mm256_set1_ps(startDir.get_y());
	startZ = _mm256_set1_ps(startDir.get_z());

	ox = _mm256_set1_ps(camPos.get_x());
	oy = _mm256_set1_ps(camPos.get_y());
	oz = _mm256_set1_ps(camPos.get_z());

	rayLen = _mm256_set1_ps(100);
}

void RaySystem::draw(Pixel* pixelBuffer) {

	// #pragma unroll
	// Initialize ray values
	for (unsigned int i = 0; i < (height * width) / AVX_SIZE; i++)
	{
		int x = i * AVX_SIZE % width;
		int y = i * AVX_SIZE / width;
		__m256 dx = _mm256_add_ps(startX,
			_mm256_setr_ps(x * xOffset, (x + 1) * xOffset, (x + 2) * xOffset, (x + 3) * xOffset,
				(x + 4) * xOffset, (x + 5) * xOffset, (x + 6) * xOffset, (x + 7) * xOffset));
		__m256 dy = _mm256_add_ps(startY,
			_mm256_set1_ps(y * yOffset));

		AvxVector3 norm = normalize(dx, dy, startZ);

		dirX[i] = norm.x;
		dirY[i] = norm.y;
		dirZ[i] = norm.z;
		originX[i] = ox;
		originY[i] = oy;
		originZ[i] = oz;
		length[i] = rayLen;
	}
	for (unsigned int j = 0; j < (height * width) / AVX_SIZE; j++)
		trace(j, 0);

	for (unsigned int j = 0; j < (height * width) / AVX_SIZE; j++)
	{
		__m256 maxColor8 = _mm256_set1_ps(255.0f);
		r[j] = _mm256_min_ps(_mm256_mul_ps(r[j], maxColor8), maxColor8);
		g[j] = _mm256_min_ps(_mm256_mul_ps(g[j], maxColor8), maxColor8);
		b[j] = _mm256_min_ps(_mm256_mul_ps(b[j], maxColor8), maxColor8);

		// Please check later if this is correct lol
		int x = j * AVX_SIZE % width;
		float y = j * AVX_SIZE / width;

		for (unsigned int c = 0; c < 8; c++) {
			unsigned int startIndex = x * 4 + (y * width) * 4;
			pixelBuffer[startIndex] = r[j].m256_f32[c];
			pixelBuffer[startIndex + 1] = g[j].m256_f32[c];
			pixelBuffer[startIndex + 2] = b[j].m256_f32[c];
			pixelBuffer[startIndex + 3] = 0;
			x += 1;
		}
	}

	/*
	Vec3Df rayStartDir = camera->getRelTopLeft();
	float xOffset = (float)SCREEN_DIMENSION * 2 / (width - 1);
	float yOffset = (float)SCREEN_DIMENSION * 2 / (height - 1);
	#pragma unroll
	for (unsigned int y = 0; y < height; y++)
	{
		for (unsigned int x = 0; x < width; x++)
		{
			// Make ray
			Vec3Df direction = rayStartDir + Vec3Df(x * xOffset, y * yOffset, 0);
			direction = normalize_vector(direction);
			Ray r = { camera->position, direction, 100 };
			// Check for hits
			Vec3Df argb = trace(r,0) * Vec3Df(255);

			// Convert color
			unsigned int xy = x * 4 + (y * width) * 4;
			*(pixelBuffer + xy) = std::min((int)argb.extract(0), 255);
			*(pixelBuffer + xy+1) = std::min((int)argb.extract(1), 255);
			*(pixelBuffer + xy+2) = std::min((int)argb.extract(2), 255);
			*(pixelBuffer + xy + 3) = 0;
		}
	};*/
}


// No idea what this should return, but probably a colorX, colorY and colorZ thats why its returning a __m256 array 
AvxVector3 RaySystem::trace(int ind, int depth)
{
	__m256 zero8 = _mm256_setzero_ps();
	AvxVector3 zeroVec = { zero8, zero8, zero8 };
	if (depth > RAYTRACER_RECURSION_DEPTH) return zeroVec;


	__m256 dx = dirX[ind];
	__m256 dy = dirY[ind];
	__m256 dz = dirZ[ind];
	__m256 ox = originX[ind];
	__m256 oy = originY[ind];
	__m256 oz = originZ[ind];
	__m256 len = length[ind];


	// Check for hits
	Ray8 r8 = { ox, oy, oz, dx, dy, dz, len };
	HitInfo8 hitInfo = HitInfo8();
	//debug to see if it hit anything
	hitInfo.matId = _mm256_set1_ps(-1);
	hitInfo.dist = _mm256_set1_ps(FLT_MAX);
	for (unsigned int i = 0; i < shapeSize; i++)
	{
		shapes[i]->hit(r8, &hitInfo);
	}
	Mat8 mat = Shape::blendMats(hitInfo.matId); 

	// distance mask
	__m256 distMask = _mm256_cmp_ps(hitInfo.dist, _mm256_set1_ps(RAYTRACER_MAX_RENDERDISTANCE), _CMP_GT_OS);

	AvxVector3 rayDir = { dx, dy, dz };
	__m256 migraine8 = _mm256_set1_ps(RAY_MIGRAINE);

	// Normal diffuse lighting for non refractive objects
	__m256 calcLightR = mat.ambientX;
	__m256 calcLightG = mat.ambientY;
	__m256 calcLightB = mat.ambientZ;
	for (Light* l : lights)
	{

		// Vec3Df lightDist = l->position - hitI.hitPos;
		const __m256 lightDistx = _mm256_sub_ps(l->posX, hitInfo.px);
		const __m256 lightDisty = _mm256_sub_ps(l->posY, hitInfo.py);
		const __m256 lightDistz = _mm256_sub_ps(l->posZ, hitInfo.pz);

		//if (dot_product(hitI.normal, lightDist) < 0)
		//	continue;
		__m256 lightMask = _mm256_cmp_ps(dot_product(hitInfo.nx, hitInfo.ny, hitInfo.nz, lightDistx, lightDisty, lightDistz), zero8, _CMP_LT_OS);

		// 	Vec3Df lightV = normalize_vector(lightDist);
		const AvxVector3 lightV = normalize(lightDistx, lightDisty, lightDistz);
		const __m256 lenLight = vector_length(lightDistx, lightDisty, lightDistz);
		//	float shadowLength = (float)(vector_length(lightDist) - 2 * RAY_MIGRAINE);
		__m256 shadowLength = _mm256_sub_ps(lenLight, _mm256_mul_ps(migraine8, _mm256_set1_ps(2)));
		// Vec3Df halfVector = normalize_vector(lightV - direction);
		const AvxVector3 halfVector = normalize(_mm256_sub_ps(lightV.x, dx), _mm256_sub_ps(lightV.y, dy), _mm256_sub_ps(lightV.z, dz));

		__m256 diffuse = _mm256_div_ps(l->intensity8, lenLight);

		//	Vec3Df blinnphong = hitI.material.diffuseColor * l->intensity * Vec3Df(std::max(0.0f, dot_product(hitI.normal, lightV)));
		__m256 blinnPhongRightSide = _mm256_max_ps(_mm256_setzero_ps(), dot_product(hitInfo.nx, hitInfo.ny, hitInfo.nz, lightV.x, lightV.y, lightV.z));
		__m256 blinnX = _mm256_mul_ps(_mm256_mul_ps(mat.diffuseX, l->intensity8), blinnPhongRightSide);
		__m256 blinnY = _mm256_mul_ps(_mm256_mul_ps(mat.diffuseY, l->intensity8), blinnPhongRightSide);
		__m256 blinnZ = _mm256_mul_ps(_mm256_mul_ps(mat.diffuseZ, l->intensity8), blinnPhongRightSide);

		//	Vec3Df specular = hitI.material.diffuseColor * hitI.material.specularColor * l->intensity;
		//	specular *= Vec3Df((float)pow(std::max(0.0f, dot_product(hitI.normal, halfVector)), BLINN_PHONG_POWER));
		
		__m256 specCoeff = _mm256_max_ps(zero8, dot_product(hitInfo.nx, hitInfo.ny, hitInfo.nz, halfVector.x, halfVector.y, halfVector.z));
		specCoeff = pow(Vec8f(specCoeff), BLINN_PHONG_POWER);
		__m256 specularX = _mm256_mul_ps(_mm256_mul_ps(_mm256_mul_ps(mat.specularX, mat.diffuseX), l->intensity8), specCoeff);
		__m256 specularY = _mm256_mul_ps(_mm256_mul_ps(_mm256_mul_ps(mat.specularY, mat.diffuseY), l->intensity8), specCoeff);
		__m256 specularZ = _mm256_mul_ps(_mm256_mul_ps(_mm256_mul_ps(mat.specularZ, mat.diffuseZ), l->intensity8), specCoeff);
		//	blinnphong += specular;
		blinnX = _mm256_add_ps(blinnX, specularX);
		blinnY = _mm256_add_ps(blinnY, specularY);
		blinnZ = _mm256_add_ps(blinnZ, specularZ);

		//	Vec3Df lightColor = diffuse * blinnphong;
		//	argb += lightColor;
		calcLightR = _mm256_add_ps(calcLightR, _mm256_blendv_ps(_mm256_mul_ps(blinnX, diffuse), zero8, lightMask));
		calcLightG = _mm256_add_ps(calcLightG, _mm256_blendv_ps(_mm256_mul_ps(blinnY, diffuse), zero8, lightMask));
		calcLightB = _mm256_add_ps(calcLightB, _mm256_blendv_ps(_mm256_mul_ps(blinnZ, diffuse), zero8, lightMask)); 
	}
	// Vec3Df normalCol = calculateLight(hitInfo, ray.direction) * hitInfo.material.diffuseColor;
	calcLightR = _mm256_mul_ps(calcLightR, mat.diffuseX);
	calcLightG = _mm256_mul_ps(calcLightG, mat.diffuseY);
	calcLightB = _mm256_mul_ps(calcLightB, mat.diffuseZ);

	// End of the lighting part


	// Refraction part
	__m256 one8 = _mm256_set1_ps(1);
	__m256 minusOne8 = _mm256_set1_ps(-1);
	__m256 matRefracIndex = mat.refracIndex;
	__m256 refracMask = _mm256_cmp_ps(matRefracIndex, one8, _CMP_GT_OS);
	if (_mm256_movemask_ps(refracMask) == 0) {
		r[ind] = _mm256_blendv_ps(calcLightR, zero8, distMask);
		g[ind] = _mm256_blendv_ps(calcLightG, zero8, distMask);
		b[ind] = _mm256_blendv_ps(calcLightB, zero8, distMask);

		return { r[ind], g[ind], b[ind] };
	}
	
	// refraction mask
	

	//	bool cond = dot_product(ray.direction, hitInfo.normal) < 0;
	__m256 dotDirNor0 = dot_product(hitInfo.nx, hitInfo.ny, hitInfo.nz, dx, dy, dz);
	__m256 hitDirMask = _mm256_cmp_ps(dotDirNor0, zero8, _CMP_LT_OS);
	
	// Vec3Df hitNormal = hitInfo.normal * (cond ? 1 : -1);
	__m256 hitDirMult = _mm256_blendv_ps(minusOne8, one8, hitDirMask);
	__m256 hitDirX = _mm256_mul_ps(hitInfo.nx, hitDirMult);
	__m256 hitDirY = _mm256_mul_ps(hitInfo.ny, hitDirMult);
	__m256 hitDirZ = _mm256_mul_ps(hitInfo.nz, hitDirMult);
	
	// float refracIndex = cond ? hitInfo.material.refracIndex : (1 / hitInfo.material.refracIndex);
	__m256 refracIndex = _mm256_blendv_ps(_mm256_rcp_ps(mat.refracIndex), mat.refracIndex, hitDirMask);
	
	// float cosTheta = rayCosTheta(ray, hitNormal, refracIndex);
	__m256 dotDirNor = dot_product(dx, dy, dz, hitDirX, hitDirY, hitDirZ);
	__m256 cosTheta = _mm256_sub_ps(one8, _mm256_div_ps(_mm256_mul_ps(_mm256_set1_ps(1.000586f), _mm256_sub_ps(one8, _mm256_mul_ps(dotDirNor, dotDirNor))), _mm256_mul_ps(refracIndex, refracIndex)));
	
	// bool refracted = cosTheta >= 0;
	__m256 refractedMask = _mm256_cmp_ps(cosTheta, zero8, _CMP_GT_OS);

	// Vec3Df refractDir = refracted ? refractRay(ray, hitNormal, refracIndex, cosTheta) : Vec3Df(0);
	// Vec3Df sinPhi = (r.refracIndex * (r.direction - normal * dot_product(r.direction, normal))) / rIndex;
	// Vec3Df refractDir = normalize_vector(sinPhi - normal * Vec3Df(sqrtf(rayCosTheta)));
	__m256 mulNormHit = _mm256_mul_ps(hitInfo.nx, dotDirNor);
	__m256 sinPhiX = _mm256_div_ps(_mm256_mul_ps(_mm256_sub_ps(dx, mulNormHit), _mm256_set1_ps(1.000293f)), refracIndex);
	__m256 sinPhiY = _mm256_div_ps(_mm256_mul_ps(_mm256_sub_ps(dy, mulNormHit), _mm256_set1_ps(1.000293f)), refracIndex);
	__m256 sinPhiZ = _mm256_div_ps(_mm256_mul_ps(_mm256_sub_ps(dz, mulNormHit), _mm256_set1_ps(1.000293f)), refracIndex);
	__m256 sqrtCosTheta = _mm256_sqrt_ps(cosTheta);
	__m256 refDirX = _mm256_sub_ps(sinPhiX, _mm256_mul_ps(hitInfo.nx, sqrtCosTheta));
	__m256 refDirY = _mm256_sub_ps(sinPhiY, _mm256_mul_ps(hitInfo.ny, sqrtCosTheta));
	__m256 refDirZ = _mm256_sub_ps(sinPhiZ, _mm256_mul_ps(hitInfo.nz, sqrtCosTheta));
	AvxVector3 refDirVec = normalize(refDirX, refDirY, refDirZ);
	__m256 refractDirX = _mm256_blendv_ps(zero8, refDirVec.x, refractedMask);
	__m256 refractDirY = _mm256_blendv_ps(zero8, refDirVec.y, refractedMask);
	__m256 refractDirZ = _mm256_blendv_ps(zero8, refDirVec.z, refractedMask);

	// trace(mirrorRay(ray.direction, hitInfo), depth + 1)
	__m256 doubleDotDirNor0 = _mm256_add_ps(dotDirNor0, dotDirNor0);
	__m256 mirrDirX = _mm256_sub_ps(dx, _mm256_mul_ps(hitInfo.nx, doubleDotDirNor0));
	__m256 mirrDirY = _mm256_sub_ps(dy, _mm256_mul_ps(hitInfo.ny, doubleDotDirNor0));
	__m256 mirrDirZ = _mm256_sub_ps(dz, _mm256_mul_ps(hitInfo.nz, doubleDotDirNor0));
	AvxVector3 mirrDir = normalize(mirrDirX, mirrDirY, mirrDirZ);
	originX[ind] = _mm256_add_ps(hitInfo.px, _mm256_mul_ps(migraine8, mirrDir.x));
	originY[ind] = _mm256_add_ps(hitInfo.py, _mm256_mul_ps(migraine8, mirrDir.y));
	originZ[ind] = _mm256_add_ps(hitInfo.pz, _mm256_mul_ps(migraine8, mirrDir.z));
	dirX[ind] = mirrDir.x;
	dirY[ind] = mirrDir.y;
	dirZ[ind] = mirrDir.z;
	length[ind] = _mm256_set1_ps(1000);
	AvxVector3 mCol = trace(ind, depth + 1);

	// trace(refractedRay, depth + 1)
	originX[ind] = _mm256_add_ps(hitInfo.px, _mm256_mul_ps(migraine8, refractDirX));
	originY[ind] = _mm256_add_ps(hitInfo.py, _mm256_mul_ps(migraine8, refractDirY));
	originZ[ind] = _mm256_add_ps(hitInfo.pz, _mm256_mul_ps(migraine8, refractDirZ));
	dirX[ind] = refractDirX;
	dirY[ind] = refractDirY;
	dirZ[ind] = refractDirZ;
	length[ind] = _mm256_set1_ps(1000);
	AvxVector3 rCol = trace(ind, depth + 1);

	// Vec3Df power = -hitInfo.material.absorbtion * refractDir;
	__m256 powerX = _mm256_mul_ps(_mm256_mul_ps(mat.absorbX, minusOne8), refractDirX);
	__m256 powerY = _mm256_mul_ps(_mm256_mul_ps(mat.absorbY, minusOne8), refractDirY);
	__m256 powerZ = _mm256_mul_ps(_mm256_mul_ps(mat.absorbZ, minusOne8), refractDirZ);

	// k = Vec3Df(pow(E, power[0]), pow(E, power[1]), pow(E, power[2]));
	__m256 e8 = _mm256_set1_ps(E);
	__m256 kx = _mm256_pow_ps(e8, powerX);
	__m256 ky = _mm256_pow_ps(e8, powerY);
	__m256 kz = _mm256_pow_ps(e8, powerZ);
	kx = _mm256_blendv_ps(kx, one8, hitDirMask);
	ky = _mm256_blendv_ps(ky, one8, hitDirMask);
	kz = _mm256_blendv_ps(kz, one8, hitDirMask);
	
	// float R0 = ((hitInfo.material.refracIndex - 1) * (hitInfo.material.refracIndex - 1)) / ((hitInfo.material.refracIndex + 1) * (hitInfo.material.refracIndex + 1));
	__m256 refIdMinusOne = _mm256_sub_ps(matRefracIndex, one8);
	__m256 refIdPlusOne = _mm256_add_ps(matRefracIndex, one8);
	__m256 R0 = _mm256_div_ps(_mm256_mul_ps(refIdMinusOne, refIdMinusOne), _mm256_mul_ps(refIdPlusOne, refIdPlusOne));

	// c = dot_product(rayDir, hitInfo.normal);
	// float c_comp = 1 - c;
	__m256 rayDirMult = _mm256_blendv_ps(one8, minusOne8, hitDirMask);
	__m256 c_comp = _mm256_sub_ps(one8, _mm256_mul_ps(dotDirNor0, rayDirMult));

	// float R = R0 + (1 - R0) * c_comp * c_comp * c_comp * c_comp * c_comp;
	__m256 c_compPow2 = _mm256_mul_ps(c_comp, c_comp);
	__m256 c_compPow5 = _mm256_mul_ps(_mm256_mul_ps(c_compPow2, c_compPow2), c_comp);
	__m256 R = _mm256_add_ps(R0, _mm256_mul_ps(_mm256_sub_ps(one8, R0), c_compPow5));

	// return k * (R * trace(mirrorRay(ray.direction, hitInfo), depth + 1)) + (1 - R) * trace(refractedRay, depth + 1);
	__m256 cx = _mm256_mul_ps(kx, _mm256_add_ps(_mm256_mul_ps(R, mCol.x), _mm256_mul_ps(_mm256_sub_ps(one8, R), rCol.x)));
	__m256 cy = _mm256_mul_ps(ky, _mm256_add_ps(_mm256_mul_ps(R, mCol.y), _mm256_mul_ps(_mm256_sub_ps(one8, R), rCol.y)));
	__m256 cz = _mm256_mul_ps(kz, _mm256_add_ps(_mm256_mul_ps(R, mCol.z), _mm256_mul_ps(_mm256_sub_ps(one8, R), rCol.z)));

	__m256 finalMask = _mm256_or_ps(hitDirMask, refractedMask);
	cx = _mm256_blendv_ps(mCol.x, cx, finalMask);
	cy = _mm256_blendv_ps(mCol.y, cy, finalMask);
	cz = _mm256_blendv_ps(mCol.z, cz, finalMask);

	//__m256 hitDirX = 
	//if (hitInfo.material.refracIndex > 1)
	//{
	//	// Init beers law
	//	Vec3Df k = Vec3Df(1);
	//	float c = 0;
	//	lastId = hitInfo.id;

	//	bool cond = dot_product(ray.direction, hitInfo.normal) < 0;
	//	Vec3Df hitNormal = hitInfo.normal * (cond ? 1 : -1);
	//	float refracIndex = cond ? hitInfo.material.refracIndex : (1 / hitInfo.material.refracIndex);
	//	// Obtuse angle
	//	float cosTheta = rayCosTheta(ray, hitNormal, refracIndex);
	//	bool refracted = cosTheta >= 0;
	//	Vec3Df refractDir = refracted ? refractRay(ray, hitNormal, refracIndex, cosTheta) : Vec3Df(0);
	//	if (!cond)
	//	{
	//		if (!refracted)
	//		{
	//			return trace(mirrorRay(ray.direction, hitInfo), depth + 1);
	//		}
	//		Vec3Df power = -hitInfo.material.absorbtion * refractDir;
	//		k = Vec3Df(pow(E, power[0]), pow(E, power[1]), pow(E, power[2]));
	//	}
	//	Ray refractedRay = { hitInfo.hitPos + refractDir * Vec3Df(RAY_MIGRAINE), refractDir, 1000 };
	//	Vec3Df rayDir = cond ? -ray.direction : ray.direction;
	//	c = dot_product(rayDir, hitInfo.normal);
	//	float R0 = ((hitInfo.material.refracIndex - 1) * (hitInfo.material.refracIndex - 1)) / ((hitInfo.material.refracIndex + 1) * (hitInfo.material.refracIndex + 1));
	//	float c_comp = 1 - c;
	//	float R = R0 + (1 - R0) * c_comp * c_comp * c_comp * c_comp * c_comp;
	//	return  k * (R * trace(mirrorRay(ray.direction, hitInfo), depth + 1)) + (1 - R) * trace(refractedRay, depth + 1);
	//}
	// end of refraction part

	cx = _mm256_blendv_ps(calcLightR, cx, refracMask);
	cy = _mm256_blendv_ps(calcLightG, cy, refracMask);
	cz = _mm256_blendv_ps(calcLightB, cz, refracMask);

	r[ind] = _mm256_blendv_ps(cx, zero8, distMask);
	g[ind] = _mm256_blendv_ps(cy, zero8, distMask);
	b[ind] = _mm256_blendv_ps(cz, zero8, distMask);
	 
	return { r[ind], g[ind], b[ind] }; 
}  
