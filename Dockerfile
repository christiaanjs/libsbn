FROM continuumio/anaconda3

COPY environment.yml .

RUN conda env create -f environment.yml
RUN conda install -n libsbn -y gxx_linux-64
