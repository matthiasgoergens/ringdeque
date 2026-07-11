from setuptools import Extension, setup

setup(
    ext_modules=[
        Extension(
            "ringdeque._ringdeque",
            sources=["src/ringdeque/_ringdeque.c"],
        )
    ]
)
