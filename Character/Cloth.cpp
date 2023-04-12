
#include "Cloth.hpp"
#include "SpringForce.hpp"
#include <cstdlib>
#include <array>

#include <chrono>

//#define CUDA

//#define VERBOSE

struct Sphere
{
	float radius = 0.f;
	Vec3 center;
};


struct Triangle {
	Particle a;
	Particle b;
	Particle c;
};

//Object storage
static std::vector<Sphere> sVector;
static std::vector<Particle*> pVector;
static std::vector<SubParticle> cudaPVector;
static std::vector<Force*> fVector;
static std::vector<std::pair<int, int>> cudaFVector;

//Consts
#define RAND (((rand()%2000)/1000.f)-1.f)
#define DAMP 0.98f
#define EPS 0.001f
#define GRA 9.8f
#define FRICTION 0.5f

//Cloth and spatial grid parameters
const int radius = 30;
const int diameter = 2 * radius + 1;
const int gridDivisions = diameter / 6;

//Global parameters
float tclock = 0.f;
bool pin = false;
bool sidePin = false;
float springConstCollision = 15;
float springConstSphere = 40;
float collisionDist = 1.5;
bool windOn = false;
bool doDrawTriangle = false;
double ks = 30.f;
double kd = 1.f;
bool tearing = false;
bool stepAhead = false;
bool spheresOn = true;
float sphereRadius = 4.f;
bool randHeight = false;




bool* GPU_simulate(static std::vector<Sphere> sVector,
	static std::vector<SubParticle>* pVector,
	static std::vector<std::pair<int,int>>* fVector);

Vector3f Vec3ToVector3f(Vec3 v) {
	return make_vector(v.x, v.y, v.z);
}

void triangleDraw(Vector3f a, Vector3f b, Vector3f c, Vector3f color) {
	Vector3f productVec = product(normalize(cross_product(a-b, c-b)), normalize(make_vector(0.f, -1.f, 1.f)));
	float colorFactor = abs(productVec.x + productVec.y + productVec.z);
	color *= (1.f/3.f+ (1.f-1.f/3.f)*colorFactor);
	glBegin(GL_TRIANGLES);
	glColor3f(color.x,color.y,color.z);
	glVertex3f(a.x,a.y,a.z);
	glColor3f(color.x, color.y, color.z);
	glVertex3f(b.x, b.y, b.z);
	glColor3f(color.x, color.y, color.z);
	glVertex3f(c.x, c.y, c.z);
	glEnd();
}

Cloth::Cloth() {
	//All that follows are variables used through out the program that can be set along with the cloth
	dt = 1.f/60.f;
	float height = 10.f;
	float normalDist = 7.5;
	double dist = normalDist / radius;
	Vec3 center = Vec3(0.0f, height, 0.0f);
	float offset = dist;
	ks = 100;
	kd = ks/100.f/pow(3.f,int(ks) / 500); //Scales kd so it is high enough to
	//have good results, while accounting for the need to lower it for higher ks
	int clothOption = 0;
	spheresOn = true;
	sphereRadius = 4.f;
	pin = true;
	sidePin = false;
	springConstCollision = 25; //Self collision seems to be broken atm
	springConstSphere = 40;
	collisionDist = 15/radius;
	randHeight = false;
	windOn = false;
	doDrawTriangle = true;
	tearing = true; //Visaulizes better without triangles being drawn
	integratorSet = SYMPLECTIC;
	//Forward is the least stable, Symplectic has the best results, 
	//Backwards leads to weak forces and is also extremely slow, 
	//Verlet might have an incorrect implementation. It doesn't explode like forward but it tends to twist quite heavily.


	int sceneSetting = 0; //Faster way of choosing var. presets

	if (sceneSetting == 0) { //Spheres
		spheresOn = true;
		clothOption = 0;
		pin = true;
		sidePin = false;
		windOn = false;
		tearing = false;
	}
	else if (sceneSetting == 1) { //Banner
		spheresOn = false;
		clothOption = 0;
		pin = false;
		sidePin = true;
		windOn = true;
		tearing = false;
	}
	else if (sceneSetting == 2) { //Pinned folded
		spheresOn = false;
		clothOption = 1;
		pin = true;
		sidePin = false;
		windOn = false;
		tearing = true;
	}
	else if (sceneSetting == 3) { //Unpinned folded
		spheresOn = false;
		clothOption = 1;
		pin = false;
		sidePin = false;
		windOn = false;
		tearing = false;
	}

	if (clothOption == 1) {
		//Creating particles (folded over)
		for (int i = -radius; i <= radius; i++) {
			for (int k = -radius; k <= radius; k++) {
				if (i <= 0) {
					Vec3 offsetVector = Vec3(offset * i, randHeight ? RAND / 2.5f : 0.f, offset * k);
					pVector.push_back(new Particle(center + offsetVector));
					SubParticle subP;
					subP.m_Position = center + offsetVector;
					subP.m_ConstructPos = center + offsetVector;
					cudaPVector.push_back(subP);
				}
				else {
					Vec3 offsetVector = Vec3(-offset * i, 5.0f + (randHeight ? RAND / 2.5f : 0.f), offset * k);
					pVector.push_back(new Particle(center + offsetVector));
					SubParticle subP;
					subP.m_Position = center + offsetVector;
					subP.m_ConstructPos = center + offsetVector;
					cudaPVector.push_back(subP);
				}

			}
		}
	}
	else {
		//Creating particles (Flat)
		for (int i = -radius; i <= radius; i++) {
			for (int k = -radius; k <= radius; k++) {
				Vec3 offsetVector(offset * i, randHeight ? RAND / 2.5f : 0.f, offset * k);
				pVector.push_back(new Particle(center + offsetVector));
				SubParticle subP;
				subP.m_Position = center + offsetVector;
				subP.m_ConstructPos = center + offsetVector;
				cudaPVector.push_back(subP);
			}
		}
	}

	float stretchTearFactor = 2.f;
	float shearTearFactor = 2.f*sqrt(2);
	float bendTearFactor = 4.f;
	//Stretch and bend Forces
	for (int i = 0; i < diameter; i++) {
		for (int k = 0; k < diameter; k++) {
			//stretch
			if (i != diameter - 1) {
				fVector.push_back(new SpringForce(&(cudaPVector[diameter * i + k]), &(cudaPVector[(diameter * (i + 1) + k)]),
					dist, ks, kd, diameter * i + k, (diameter * (i + 1) + k), i, k, stretchTearFactor));
				cudaFVector.push_back(std::make_pair(diameter * i + k, (diameter * (i + 1) + k)));
			}
			if (k != diameter - 1){
				fVector.push_back(new SpringForce(&(cudaPVector[diameter * i + k]), &(cudaPVector[diameter * i + (k + 1)]),
					dist, ks, kd, diameter * i + k, diameter * i + (k + 1), i, k, stretchTearFactor));
				cudaFVector.push_back(std::make_pair(diameter * i + k, diameter * i + (k + 1)));
			}
			//bend
			if (i != diameter - 2 && i < diameter - 1) {
				fVector.push_back(new SpringForce(&(cudaPVector[diameter * i + k]), &(cudaPVector[(diameter * (i + 2) + k)]),
					dist * 2.f, ks / 2.f, kd * 2.f, diameter * i + k, diameter * (i + 2) + k, i, k, bendTearFactor));
				cudaFVector.push_back(std::make_pair(diameter * i + k, diameter * (i + 2) + k));
			}
			if (k != diameter - 2 && k < diameter - 1) {
				fVector.push_back(new SpringForce(&(cudaPVector[diameter * i + k]), &(cudaPVector[diameter * i + (k + 2)]),
					dist * 2.f, ks / 2.f, kd * 2.f, diameter * i + k, diameter * i + (k + 2), i, k, bendTearFactor));
				cudaFVector.push_back(std::make_pair(diameter * i + k, diameter * i + (k + 2)));
			}
		}
	}

	//Shear forces
	//TL->BR from i
	for (int i = 0; i < diameter - 1; i++) {
		for (int offset = 0; offset < diameter - i - 1; offset++) {
			fVector.push_back(new SpringForce(&(cudaPVector[diameter * (i + offset) + offset]), &(cudaPVector[diameter * (i + offset + 1) + offset + 1]), 
				dist, ks, kd, diameter * (i + offset) + offset, diameter * (i + offset + 1) + offset + 1, i + offset, offset, shearTearFactor));
			cudaFVector.push_back(std::make_pair(diameter * (i + offset) + offset, diameter * (i + offset + 1) + offset + 1));
		}
	}
	//TL->BR from k
	for (int k = 1; k < diameter - 1; k++) {
		for (int offset = 0; offset < diameter - k - 1; offset++) {
			fVector.push_back(new SpringForce(&(cudaPVector[diameter * (offset)+k + offset]), &(cudaPVector[diameter * (offset + 1) + k + offset + 1]), 
				dist, ks, kd, diameter * (offset)+k + offset, diameter * (offset + 1) + k + offset + 1,offset, k + offset, shearTearFactor));
			cudaFVector.push_back(std::make_pair(diameter* (offset)+k + offset, diameter* (offset + 1) + k + offset + 1));
		}
	}
	//TR->BL from i
	for (int i = 0; i < diameter - 1; i++) {
		for (int offset = 0; offset < i; offset++) {
			int koffset = i - offset;
			fVector.push_back(new SpringForce(&(cudaPVector[diameter * (offset)+koffset]), &(cudaPVector[diameter * (offset + 1) + koffset - 1]), 
				dist, ks, kd, diameter * (offset)+koffset, diameter * (offset + 1) + koffset - 1,offset, koffset, shearTearFactor));
			cudaFVector.push_back(std::make_pair(diameter* (offset)+koffset, diameter* (offset + 1) + koffset - 1));
		}
	}
	//TR->BL from k
	for (int k = diameter - 2; k > 0; k--) {
		for (int offset = 0; offset < diameter - k - 1; offset++) {
			int koffset = k + offset;
			int ioffset = diameter - 1 - offset;
			fVector.push_back(new SpringForce(&(cudaPVector[diameter * (ioffset)+koffset]), &(cudaPVector[diameter * (ioffset - 1) + koffset + 1]), 
				dist, ks, kd, diameter* (ioffset)+koffset, diameter* (ioffset - 1) + koffset + 1,ioffset, offset, shearTearFactor));
			cudaFVector.push_back(std::make_pair(diameter* (ioffset)+koffset, diameter* (ioffset - 1) + koffset + 1));
		}
	}



	if (spheresOn) {
		//Adding spheres
		Sphere sphere0;
		sphere0.radius = sphereRadius;
		sphere0.center = Vec3(-4.f, 2.f, -4.f);
		sVector.push_back(sphere0);
		Sphere sphere1;
		sphere1.radius = sphereRadius;
		sphere1.center = Vec3(4.f, 2.f, -4.f);
		sVector.push_back(sphere1);
		Sphere sphere2;
		sphere2.radius = sphereRadius;
		sphere2.center = Vec3(-4.f, 2.f, 4.f);
		sVector.push_back(sphere2);
		Sphere sphere3;
		sphere3.radius = sphereRadius;
		sphere3.center = Vec3(4.f, 2.f, 4.f);
		sVector.push_back(sphere3);
	}

}

Cloth::~Cloth(){
	cudaPVector.clear();
	sVector.clear();
	cudaFVector.clear();
	pVector.clear();
	fVector.clear();
}

void Cloth::reset(){
	tclock = 0.f;
	int size = pVector.size();
	for(int ii=0; ii<size; ii++){
		pVector[ii]->reset();
		cudaPVector[ii].reset();
	}
}

void Cloth::draw(){
	if (!doDrawTriangle) {
		int size = pVector.size();
		for (int ii = 0; ii < size; ii++) {
			cudaPVector[ii].draw();
		}

		size = fVector.size();
		for (int ii = 0; ii < size; ii++) {
			fVector[ii]->draw();
		}
	}
	else {
		for (int i = 0; i < diameter - 1; i++) {
			for (int k = 0; k < diameter - 1; k++) {
				Vector3f color = make_vector(1.f, 0.f, 0.f); //Ensure both have similar noemals by ordering as such
				triangleDraw(Vec3ToVector3f(cudaPVector[i * diameter + k].m_Position), 
							 Vec3ToVector3f(cudaPVector[(i + 1) * diameter + k].m_Position),
							 Vec3ToVector3f(cudaPVector[i * diameter + k + 1].m_Position), color);
				triangleDraw(Vec3ToVector3f(cudaPVector[(i + 1) * diameter + k].m_Position),
					         Vec3ToVector3f(cudaPVector[(i + 1) * diameter + k + 1].m_Position),
					         Vec3ToVector3f(cudaPVector[i * diameter + k + 1].m_Position), color);
			}
		}
	}

}

void Cloth::simulation_step(){

#ifdef CUDA
	
	



	bool* tornVec = GPU_simulate(sVector, &cudaPVector, &cudaFVector);
	size_t tornLen = fVector.size();

#endif // CUDA

#ifndef CUDA
	cpu_simulate();
#endif // !CUDA


	if (!stepAhead) {
		///Then, we can move forward
		///Change this to others if you want to implement RK2 or RK4 or other integration method
		euler_step(integratorSet);

		//Floor collision
		for (auto& p : cudaPVector) {
			if (p.m_Position.y < EPS) { //EPS used to avoid z-fighting
				p.m_Position = Vec3(p.m_Position.x, EPS, p.m_Position.z);
			}
		}
	}


}

void Cloth::euler_step(Integrator integrator){
	//Symplectic Euler
	if (integrator == SYMPLECTIC) {
		for (auto& p : cudaPVector) {
			p.m_Velocity = Vec3(DAMP) * p.m_Velocity + Vec3(DAMP) * p.m_ForceAccumulator * dt;
			p.m_Position += Vec3(dt) * p.m_Velocity;
		}
	}
	else if (integrator == VERLET) {
		for (int i = 0; i < cudaPVector.size(); i++) {
			auto& p = cudaPVector[i];
			auto& pFull = pVector[i];
			Vec3 tempPos = Vec3(p.m_Position.x, p.m_Position.y, p.m_Position.z);
			p.m_Position = Vec3(2) * p.m_Position - pFull->m_LastPosition + Vec3(dt) * Vec3(dt) * p.m_ForceAccumulator * DAMP;
			pFull->m_LastPosition = tempPos;
			
		}
	}
	else if (integrator == BACKWARD) {
		for (auto& p : cudaPVector) {
			stepAhead = true;
			simulation_step();
			p.m_Velocity = Vec3(DAMP) * p.m_Velocity + Vec3(DAMP) * p.m_ForceAccumulator * dt;
			stepAhead = false;
			p.m_Position = p.m_Position + Vec3(dt) * p.m_Velocity;
		}

	}
	else {
		for (auto& p : cudaPVector) {
			p.m_Position += p.m_Velocity * dt;
			p.m_Velocity = p.m_Velocity * DAMP + p.m_ForceAccumulator * dt * DAMP;
		}
	}
	tclock += dt;
}




void Cloth::cpu_simulate() {

	auto start_t = std::chrono::high_resolution_clock::now();

	//Wind force information
	Vec3 windDir = Vec3(1.f, 0.f, 0.f);
	auto windMagnitude = [this](Vec3 pos) {
		float x = pos.x; float y = pos.y; float z = pos.z;
		return 7.f * (cos(tclock * 10.f) + 1.f) * abs(sin(z + tclock * 5) + cos(y + tclock * 5) / 3.f);
	};

	auto particle_start = std::chrono::high_resolution_clock::now();
	//Clear force accumulators for all particles and then apply gravity and then wind and sphere forces
	for (auto& pMini : cudaPVector) {
		pMini.clearForce();
		float gOffset = -GRA;
		pMini.m_ForceAccumulator = pMini.m_ForceAccumulator 
			+ Vec3(0, gOffset, 0);
		if (windOn) pMini.m_ForceAccumulator = 
			pMini.m_ForceAccumulator + windDir * windMagnitude(pMini.m_Position);

		//Sphere collisions via spring forces
		for (auto& s : sVector) {
			if (vecNorm(pMini.m_Position - s.center) < s.radius) {
				SubParticle tempSphereParticle(Vec3(0.f));
				SpringForce collideForce(&pMini, &tempSphereParticle, (2.f + radius / 40.f) * s.radius,
					ks != 0.f && ks < 50 ? springConstSphere * (5.8f / (sqrt(ks))) : springConstSphere, //scale sphere ks to allow functioning at lower cloth ks's (10-50)
					(2.f + radius / 40.f) * s.radius, 0, 0, INFINITY); //Cannot find a good values for ks < 10
				collideForce.apply_force(); //Distance const scaled by radii to avoid clipping
			}
		}

		if (pMini.m_Position.y <= EPS) {
			//Apply friction force
			Vec3 frictionVelVector = Vec3(-pMini.m_Velocity.x, 0.f, -pMini.m_Velocity.z);
			Vec3 frictionVector = vecNormalize(frictionVelVector);
			float frictionVel = vecNorm(frictionVelVector);
			pMini.m_ForceAccumulator = pMini.m_ForceAccumulator +  frictionVelVector * FRICTION;
		}
	}
	auto particle_end = std::chrono::high_resolution_clock::now();
	
	auto f_start = std::chrono::high_resolution_clock::now();

	//Apply spring forces and tearing if need be
	std::vector<std::vector<Force*>::iterator> eraseForceList;
	for (auto& f = fVector.end(); f != fVector.begin(); f--) { //Listing backwards avoids indexing errors
		auto& fPred = f - 1;
		(*fPred)->apply_force();
		if ((*fPred)->willTear() && tearing && !stepAhead) {
			eraseForceList.push_back(fPred);
		}
	}
	auto f_end = std::chrono::high_resolution_clock::now();
	if (tearing && !stepAhead) {
		for (auto& f = eraseForceList.begin(); f != eraseForceList.end(); f++) {
			fVector.erase(*f);
		}
	}
	
	


	//Pin corners
	if (pin) {
		cudaPVector[0].m_ForceAccumulator = Vec3(0.f, 0.f, 0.f);
		cudaPVector[diameter - 1].m_ForceAccumulator = Vec3(0.f, 0.f, 0.f);
		cudaPVector[(diameter * diameter) - 1].m_ForceAccumulator = Vec3(0.f, 0.f, 0.f);
		cudaPVector[diameter * (diameter - 1)].m_ForceAccumulator = Vec3(0.f, 0.f, 0.f);
	}
	else if (sidePin) {
		//Pin a whole side of the cloth
		for (size_t i = 0; i < diameter; i++) {
			cudaPVector[i].m_ForceAccumulator = Vec3(0.f, 0.f, 0.f);
		}
	}

	auto end_t = std::chrono::high_resolution_clock::now();
#ifdef VERBOSE


	std::chrono::duration<double>  dif_t = end_t - start_t;
	std::chrono::duration<double>  particle_dif = particle_end - particle_start;
	std::chrono::duration<double>  f_dif = f_end - f_start;
	std::cout << "Time deltas: \n" << "Particles: " << particle_dif.count() << 
		"\n" << "Forces and Tearing: " << f_dif.count() << "\nTotal: " << dif_t.count() << std::endl;
#endif //  VERBOSE
}